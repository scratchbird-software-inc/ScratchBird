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

struct TxExpected {
  std::string_view dialect;
  std::string_view release_profile;
  std::string_view compatibility_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view transaction_session_profile;
  std::string_view statement_family;
  std::string_view operation_family;
  std::string_view transaction_session_surface;
  std::string_view statement_family_linkage;
  std::string_view begin_autocommit_policy;
  std::string_view isolation_read_only_deferrable_descriptor_policy;
  std::string_view session_variable_sql_mode_descriptor_policy;
  bool begin_surface{false};
  bool commit_surface{false};
  bool rollback_surface{false};
  bool rollback_to_savepoint_surface{false};
  bool savepoint_surface{false};
  bool release_savepoint_surface{false};
  bool autocommit_surface{false};
  bool isolation_descriptor_surface{false};
  bool read_only_surface{false};
  bool read_write_surface{false};
  bool wait_no_wait_surface{false};
  bool deferrable_surface{false};
  bool session_variable_surface{false};
  bool sql_mode_surface{false};
  bool statement_timeout_surface{false};
  bool search_path_surface{false};
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
                 std::string(label) + " leaked transaction/session source marker: " +
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
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready", label);
  ok &= Expect(Contains(payload, "enterprise_implemented_proven\":false"),
               std::string(label) + " missing enterprise implementation proof");
  return ok;
}

bool ExpectEnvelopeAuthority(std::string_view payload,
                             const TxExpected& expected,
                             std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"envelope\":\"SBLRExecutionEnvelope.v3\""),
               std::string(label) + " missing SBLR envelope");
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "operation_family", expected.operation_family, label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectBool(payload, "reference_engine_sql_executed", false, label);
  ok &= ExpectBool(payload, "sql_text_included", false, label);
  ok &= ExpectReadinessComplete(payload, label);
  return ok;
}

bool ExpectTransactionSessionEvidence(std::string_view payload,
                                      const TxExpected& expected,
                                      std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"transaction_session_semantic_evidence\":{"),
               std::string(label) + " missing transaction/session evidence");
  ok &= ExpectField(
      payload, "evidence_contract",
      "compatibility_transaction_session_semantic_descriptor_evidence.v1", label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "compatibility_profile_uuid",
                    expected.compatibility_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid",
                    expected.semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "release_profile", expected.release_profile, label);
  ok &= ExpectField(payload, "transaction_session_profile",
                    expected.transaction_session_profile, label);
  ok &= ExpectField(payload, "transaction_session_surface",
                    expected.transaction_session_surface, label);
  ok &= ExpectField(payload, "statement_family_linkage",
                    expected.statement_family_linkage, label);
  ok &= ExpectField(payload, "begin_autocommit_policy",
                    expected.begin_autocommit_policy, label);
  ok &= ExpectField(payload, "commit_rollback_finality_policy",
                    "engine_mga_authority", label);
  ok &= ExpectField(payload, "transaction_identity_policy",
                    "engine_mga_authority", label);
  ok &= ExpectField(payload, "visibility_policy", "engine_mga_authority", label);
  ok &= ExpectField(payload, "recovery_policy", "engine_mga_authority", label);
  ok &= ExpectField(payload, "savepoint_policy",
                    "transaction_local_engine_owned", label);
  ok &= ExpectField(payload, "isolation_read_only_deferrable_descriptor_policy",
                    expected.isolation_read_only_deferrable_descriptor_policy,
                    label);
  ok &= ExpectField(payload, "session_variable_sql_mode_descriptor_policy",
                    expected.session_variable_sql_mode_descriptor_policy, label);
  ok &= ExpectBool(payload, "begin_surface", expected.begin_surface, label);
  ok &= ExpectBool(payload, "commit_surface", expected.commit_surface, label);
  ok &= ExpectBool(payload, "rollback_surface", expected.rollback_surface, label);
  ok &= ExpectBool(payload, "rollback_to_savepoint_surface",
                   expected.rollback_to_savepoint_surface, label);
  ok &= ExpectBool(payload, "savepoint_surface", expected.savepoint_surface, label);
  ok &= ExpectBool(payload, "release_savepoint_surface",
                   expected.release_savepoint_surface, label);
  ok &= ExpectBool(payload, "autocommit_surface",
                   expected.autocommit_surface, label);
  ok &= ExpectBool(payload, "isolation_descriptor_surface",
                   expected.isolation_descriptor_surface, label);
  ok &= ExpectBool(payload, "read_only_surface",
                   expected.read_only_surface, label);
  ok &= ExpectBool(payload, "read_write_surface",
                   expected.read_write_surface, label);
  ok &= ExpectBool(payload, "wait_no_wait_surface",
                   expected.wait_no_wait_surface, label);
  ok &= ExpectBool(payload, "deferrable_surface",
                   expected.deferrable_surface, label);
  ok &= ExpectBool(payload, "session_variable_surface",
                   expected.session_variable_surface, label);
  ok &= ExpectBool(payload, "sql_mode_surface", expected.sql_mode_surface, label);
  ok &= ExpectBool(payload, "statement_timeout_surface",
                   expected.statement_timeout_surface, label);
  ok &= ExpectBool(payload, "search_path_surface",
                   expected.search_path_surface, label);
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
  ok &= ExpectBool(payload, "parser_savepoint_authority", false, label);
  ok &= ExpectBool(payload, "parser_isolation_authority", false, label);
  ok &= ExpectBool(payload, "parser_autocommit_authority", false, label);
  ok &= ExpectBool(payload, "compatibility_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(
      payload, "descriptor_exactness_status",
      "parser_transaction_session_descriptor_recorded_runtime_equivalence_pending",
      label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready", label);
  ok &= Expect(!Contains(payload, "parser_storage_authority\":true"),
               std::string(label) + " granted parser storage authority");
  ok &= Expect(!Contains(payload, "parser_transaction_authority\":true"),
               std::string(label) + " granted parser transaction authority");
  ok &= Expect(!Contains(payload, "parser_transaction_finality_authority\":true"),
               std::string(label) + " granted parser transaction finality authority");
  ok &= Expect(!Contains(payload, "parser_visibility_authority\":true"),
               std::string(label) + " granted parser visibility authority");
  ok &= Expect(!Contains(payload, "compatibility_sql_executed\":true"),
               std::string(label) + " executed compatibility SQL");
  return ok;
}

template <typename Result>
bool ExpectDirectTransactionSessionResult(
    const Result& result,
    const TxExpected& expected,
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
  if (expected.operation_family.find("rollback_to_savepoint") !=
          std::string_view::npos &&
      expected.dialect != "firebird") {
    ok &= Expect(result.sblr_operation == "SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT",
                 std::string(label) +
                     " did not route rollback-to-savepoint to exact SBLR opcode");
    ok &= Expect(result.engine_api_function == "EngineRollbackToSavepoint",
                 std::string(label) +
                     " did not route rollback-to-savepoint to exact engine API");
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
  ok &= ExpectTransactionSessionEvidence(result.parser_evidence_json, expected,
                                         label);
  ok &= ExpectEnvelopeAuthority(result.sblr_envelope, expected, label);
  ok &= ExpectTransactionSessionEvidence(result.sblr_envelope, expected, label);
  ok &= ExpectNoMarkers(result.parser_evidence_json, markers, label);
  ok &= ExpectNoMarkers(result.sblr_envelope, markers, label);
  ok &= ExpectNoMysqlLts(result.parser_evidence_json, label);
  ok &= ExpectNoMysqlLts(result.sblr_envelope, label);
  return ok;
}

template <typename UdrResult>
bool ExpectUdrTransactionSessionResult(
    const UdrResult& result,
    const TxExpected& expected,
    std::span<const std::string_view> markers,
    std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelopeAuthority(result.payload, expected, label);
  ok &= ExpectTransactionSessionEvidence(result.payload, expected, label);
  if (expected.operation_family.find("rollback_to_savepoint") !=
          std::string_view::npos &&
      expected.dialect != "firebird") {
    ok &= Expect(Contains(result.payload,
                          "\"sblr_operation\":\"SBLR_TRANSACTION_ROLLBACK_TO_SAVEPOINT\""),
                 std::string(label) +
                     " UDR payload did not carry exact rollback-to-savepoint SBLR opcode");
    ok &= Expect(Contains(result.payload,
                          "\"engine_api_function\":\"EngineRollbackToSavepoint\""),
                 std::string(label) +
                     " UDR payload did not carry exact rollback-to-savepoint engine API");
  }
  ok &= ExpectNoMarkers(result.payload, markers, label);
  ok &= ExpectNoMysqlLts(result.payload, label);
  return ok;
}

template <typename DirectParser, typename UdrParser>
bool RunTransactionSessionCase(DirectParser direct_parser,
                               UdrParser udr_parser,
                               std::string_view sql,
                               const TxExpected& expected,
                               std::span<const std::string_view> markers,
                               std::string_view label) {
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;
  ok &= ExpectDirectTransactionSessionResult(direct_parser(sql), expected,
                                             markers,
                                             std::string(label) + " direct");
  ok &= ExpectUdrTransactionSessionResult(udr_parser(sql, trusted), expected,
                                          markers,
                                          std::string(label) + " UDR");
  return ok;
}

std::span<const std::string_view> FirebirdMarkers() {
  static constexpr std::string_view markers[] = {
      "fb_tx_secret_marker", "FB_TX_SECRET_MARKER"};
  return markers;
}

std::span<const std::string_view> MysqlMarkers() {
  static constexpr std::string_view markers[] = {
      "mysql_tx_secret_marker", "MYSQL_TX_SECRET_MARKER",
      "mysql_tx_literal_marker", "MYSQL_TX_LITERAL_MARKER"};
  return markers;
}

std::span<const std::string_view> PostgresqlMarkers() {
  static constexpr std::string_view markers[] = {
      "pg_tx_secret_marker", "PG_TX_SECRET_MARKER",
      "pg_tx_literal_marker", "PG_TX_LITERAL_MARKER"};
  return markers;
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  bool ok = true;
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "set transaction read only wait isolation level snapshot",
      {"firebird", "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1600-7000-8000-000000000302",
       "firebird.transaction_session_semantics_profile",
       "transaction", "firebird.transaction.set_transaction",
       "firebird_set_transaction_read_only_wait_isolation", "transaction",
       "firebird_set_transaction_requests_engine_mga_transaction_handle",
       "firebird_set_transaction_access_isolation_wait_descriptor_engine_enforced",
       "firebird_no_sql_mode_session_variable_transaction_surface",
       true, false, false, false, false, false, false, true, true, false,
       true, false, false, false, false, false},
      FirebirdMarkers(), "firebird set transaction read only wait snapshot");
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "commit retaining",
      {"firebird", "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1600-7000-8000-000000000302",
       "firebird.transaction_session_semantics_profile",
       "transaction", "firebird.transaction.control",
       "firebird_commit_retaining", "transaction",
       "firebird_existing_engine_transaction_required",
       "firebird_transaction_control_does_not_change_isolation_descriptor",
       "firebird_no_sql_mode_session_variable_transaction_surface",
       false, true, false, false, false, false, false, false, false, false,
       false, false, false, false, false, false},
      FirebirdMarkers(), "firebird commit retaining");
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "rollback work to savepoint fb_tx_secret_marker",
      {"firebird", "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1600-7000-8000-000000000302",
       "firebird.transaction_session_semantics_profile",
       "transaction", "firebird.transaction.rollback_to_savepoint",
       "firebird_rollback_to_savepoint", "transaction",
       "firebird_existing_engine_transaction_required",
       "firebird_transaction_control_does_not_change_isolation_descriptor",
       "firebird_no_sql_mode_session_variable_transaction_surface",
       false, false, false, true, false, false, false, false, false, false,
       false, false, false, false, false, false},
      FirebirdMarkers(), "firebird rollback to savepoint");
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "release savepoint fb_tx_secret_marker",
      {"firebird", "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1600-7000-8000-000000000302",
       "firebird.transaction_session_semantics_profile",
       "transaction", "firebird.transaction.savepoint",
       "firebird_release_savepoint", "transaction",
       "firebird_existing_engine_transaction_required",
       "firebird_transaction_control_does_not_change_isolation_descriptor",
       "firebird_no_sql_mode_session_variable_transaction_surface",
       false, false, false, false, false, true, false, false, false, false,
       false, false, false, false, false, false},
      FirebirdMarkers(), "firebird release savepoint");
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  bool ok = true;
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "start transaction read write",
      {"mysql", "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1600-7000-8000-000000000303",
       "mysql.transaction_session_semantics_profile",
       "transaction", "mysql.transaction.start",
       "mysql_start_transaction_read_write", "transaction",
       "mysql_explicit_begin_requests_engine_mga_transaction_handle_autocommit_suspended",
       "mysql_transaction_access_mode_isolation_descriptor_engine_enforced",
       "mysql_session_transaction_descriptor_engine_applies",
       true, false, false, false, false, false, false, false, false, true,
       false, false, false, false, false, false},
      MysqlMarkers(), "mysql start transaction read write");
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "set autocommit = 0",
      {"mysql", "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1600-7000-8000-000000000303",
       "mysql.transaction_session_semantics_profile",
       "session", "mysql.session.set",
       "mysql_set_autocommit", "session",
       "mysql_autocommit_session_descriptor_engine_transaction_profile",
       "mysql_transaction_access_mode_isolation_descriptor_engine_enforced",
       "mysql_autocommit_session_descriptor_engine_applies",
       false, false, false, false, false, false, true, false, false, false,
       false, false, true, false, false, false},
      MysqlMarkers(), "mysql set autocommit");
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "set session transaction isolation level serializable",
      {"mysql", "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1600-7000-8000-000000000303",
       "mysql.transaction_session_semantics_profile",
       "session", "mysql.session.set",
       "mysql_set_session_transaction_isolation", "session",
       "mysql_existing_engine_transaction_or_session_descriptor",
       "mysql_transaction_access_mode_isolation_descriptor_engine_enforced",
       "mysql_session_transaction_descriptor_engine_applies",
       false, false, false, false, false, false, false, true, false, false,
       false, false, false, false, false, false},
      MysqlMarkers(), "mysql set session transaction isolation");
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "set sql_mode = 'mysql_tx_literal_marker,ANSI_QUOTES'",
      {"mysql", "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1600-7000-8000-000000000303",
       "mysql.transaction_session_semantics_profile",
       "session", "mysql.session.set",
       "mysql_set_sql_mode", "session",
       "mysql_existing_engine_transaction_or_session_descriptor",
       "mysql_transaction_access_mode_isolation_descriptor_engine_enforced",
       "mysql_sql_mode_session_descriptor_uuid_profile_engine_applies",
       false, false, false, false, false, false, false, false, false, false,
       false, false, true, true, false, false},
      MysqlMarkers(), "mysql set sql_mode");
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "rollback work to savepoint mysql_tx_secret_marker",
      {"mysql", "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1600-7000-8000-000000000303",
       "mysql.transaction_session_semantics_profile",
       "transaction", "mysql.transaction.rollback_to_savepoint",
       "mysql_rollback_to_savepoint", "transaction",
       "mysql_existing_engine_transaction_or_session_descriptor",
       "mysql_transaction_access_mode_isolation_descriptor_engine_enforced",
       "mysql_session_transaction_descriptor_engine_applies",
       false, false, false, true, false, false, false, false, false, false,
       false, false, false, false, false, false},
      MysqlMarkers(), "mysql rollback to savepoint");
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::sbu_postgresql_parse_to_sblr;
  bool ok = true;
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "begin",
      {"postgresql", "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1600-7000-8000-000000000304",
       "postgresql.transaction_session_semantics_profile",
       "transaction", "postgresql.transaction.begin",
       "postgresql_begin", "transaction",
       "postgresql_explicit_begin_requests_engine_mga_transaction_handle",
       "postgresql_isolation_read_only_deferrable_descriptor_engine_enforced",
       "postgresql_session_guc_descriptor_engine_applies",
       true, false, false, false, false, false, false, false, false, false,
       false, false, false, false, false, false},
      PostgresqlMarkers(), "postgresql begin");
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "set transaction isolation level serializable read only deferrable",
      {"postgresql", "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1600-7000-8000-000000000304",
       "postgresql.transaction_session_semantics_profile",
       "session", "postgresql.session.set",
       "postgresql_set_transaction_serializable_read_only_deferrable",
       "transaction",
       "postgresql_existing_engine_transaction_or_session_descriptor",
       "postgresql_isolation_read_only_deferrable_descriptor_engine_enforced",
       "postgresql_session_guc_descriptor_engine_applies",
       false, false, false, false, false, false, false, true, true, false,
       false, true, false, false, false, false},
      PostgresqlMarkers(), "postgresql set transaction serializable read only");
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "rollback transaction to savepoint pg_tx_secret_marker",
      {"postgresql", "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1600-7000-8000-000000000304",
       "postgresql.transaction_session_semantics_profile",
       "transaction", "postgresql.transaction.rollback_to_savepoint",
       "postgresql_rollback_to_savepoint", "transaction",
       "postgresql_existing_engine_transaction_or_session_descriptor",
       "postgresql_isolation_read_only_deferrable_descriptor_engine_enforced",
       "postgresql_session_guc_descriptor_engine_applies",
       false, false, false, true, false, false, false, false, false, false,
       false, false, false, false, false, false},
      PostgresqlMarkers(), "postgresql rollback to savepoint");
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "set local transaction_isolation = 'pg_tx_literal_marker'",
      {"postgresql", "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1600-7000-8000-000000000304",
       "postgresql.transaction_session_semantics_profile",
       "session", "postgresql.session.set",
       "postgresql_set_local_transaction_isolation", "session",
       "postgresql_existing_engine_transaction_or_session_descriptor",
       "postgresql_isolation_read_only_deferrable_descriptor_engine_enforced",
       "postgresql_transaction_isolation_guc_descriptor_engine_applies",
       false, false, false, false, false, false, false, true, false, false,
       false, false, true, false, false, false},
      PostgresqlMarkers(), "postgresql set local transaction isolation");
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "set statement_timeout = 'pg_tx_literal_marker'",
      {"postgresql", "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1600-7000-8000-000000000304",
       "postgresql.transaction_session_semantics_profile",
       "session", "postgresql.session.set",
       "postgresql_set_statement_timeout", "session",
       "postgresql_existing_engine_transaction_or_session_descriptor",
       "postgresql_isolation_read_only_deferrable_descriptor_engine_enforced",
       "postgresql_statement_timeout_descriptor_engine_applies",
       false, false, false, false, false, false, false, false, false, false,
       false, false, true, false, true, false},
      PostgresqlMarkers(), "postgresql set statement timeout");
  ok &= RunTransactionSessionCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "set search_path to pg_tx_secret_marker",
      {"postgresql", "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1600-7000-8000-000000000304",
       "postgresql.transaction_session_semantics_profile",
       "session", "postgresql.session.set",
       "postgresql_set_search_path", "session",
       "postgresql_existing_engine_transaction_or_session_descriptor",
       "postgresql_isolation_read_only_deferrable_descriptor_engine_enforced",
       "postgresql_search_path_descriptor_uuid_profile_engine_applies",
       false, false, false, false, false, false, false, false, false, false,
       false, false, true, false, false, true},
      PostgresqlMarkers(), "postgresql set search_path");
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
