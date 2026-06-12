// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cassandra_dialect.hpp"
#include "clickhouse_dialect.hpp"
#include "mongodb_dialect.hpp"
#include "neo4j_dialect.hpp"
#include "opensearch_dialect.hpp"
#include "opensearch_sql_ppl_dialect.hpp"
#include "redis_dialect.hpp"
#include "dolt_dialect.hpp"
#include "apache_ignite_dialect.hpp"
#include "tikv_dialect.hpp"
#include "foundationdb_dialect.hpp"
#include "immudb_dialect.hpp"
#include "xtdb_dialect.hpp"
#include "influxdb_dialect.hpp"
#include "milvus_dialect.hpp"
#include "duckdb_dialect.hpp"
#include "firebird_dialect.hpp"
#include "mariadb_dialect.hpp"
#include "mysql_dialect.hpp"
#include "postgresql_dialect.hpp"
#include "sqlite_dialect.hpp"
#include "tidb_dialect.hpp"
#include "vitess_dialect.hpp"
#include "cockroachdb_dialect.hpp"
#include "yugabytedb_dialect.hpp"
#include "compatibility_worker_session.hpp"

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

template <typename Result>
bool ExpectCommonEnvelope(const Result& result,
                          std::string_view dialect,
                          std::string_view operation_family) {
  return Expect(result.ok, "parse result was not ok") &&
         Expect(result.operation_family == operation_family,
                "operation_family mismatch") &&
         Expect(Contains(result.sblr_envelope, "SBLRExecutionEnvelope.v3"),
                "missing SBLR envelope") &&
         Expect(Contains(result.sblr_envelope, "\"dialect\":\"" + std::string(dialect) + "\""),
                "missing dialect") &&
         Expect(Contains(result.sblr_envelope,
                         "\"operation_family\":\"" + std::string(operation_family) + "\""),
                "missing operation family") &&
         Expect(Contains(result.sblr_envelope, "\"descriptor_resolution\":\"uuid_required\""),
                "missing UUID descriptor rule") &&
         Expect(Contains(result.sblr_envelope, "\"engine_authority\":\"scratchbird\""),
                "missing engine authority") &&
         Expect(Contains(result.sblr_envelope, "\"reference_engine_sql_executed\":false"),
                "compatibility SQL execution was not denied") &&
         Expect(Contains(result.sblr_envelope, "\"real_reference_file_effects\":false") ||
                    Contains(result.sblr_envelope, "\"real_firebird_file_effects\":false"),
                "real compatibility file effects were not denied") &&
         Expect(Contains(result.sblr_envelope, "\"sql_text_included\":false"),
                "SBLR envelope leaked SQL text");
}

bool ExpectDiagnostic(std::string_view message_vector,
                      std::string_view diagnostic_code,
                      std::string_view label) {
  if (Contains(message_vector, diagnostic_code)) return true;
  std::cerr << label << " missing diagnostic " << diagnostic_code
            << ": " << message_vector << '\n';
  return false;
}

template <typename Result>
bool ExpectParserEvidence(const Result& result,
                          std::string_view dialect,
                          std::string_view statement_kind) {
  return Expect(Contains(result.parser_evidence_json,
                         "\"dialect\":\"" + std::string(dialect) + "\""),
                "parser evidence missing dialect") &&
         Expect(Contains(result.parser_evidence_json,
                         "\"statement_kind\":\"" + std::string(statement_kind) + "\""),
                "parser evidence missing statement kind") &&
         Expect(Contains(result.parser_evidence_json,
                         "\"cst_materialized\":true"),
                "parser evidence missing CST materialization") &&
         Expect(Contains(result.parser_evidence_json,
                         "\"ast_materialized\":true"),
                "parser evidence missing AST materialization") &&
         Expect(Contains(result.parser_evidence_json,
                         "\"bound_ast_materialized\":true"),
                "parser evidence missing BoundAST materialization") &&
         Expect(Contains(result.parser_evidence_json,
                         "\"source_text_redacted\":true"),
                "parser evidence missing source redaction") &&
         Expect(Contains(result.parser_evidence_json,
                         "\"descriptor_uuid_required\":true"),
                "parser evidence missing UUID descriptor requirement") &&
         Expect(Contains(result.parser_evidence_json,
                         "\"parser_transaction_finality_authority\":false"),
                "parser evidence claims transaction finality authority") &&
         Expect(Contains(result.parser_evidence_json,
                         "\"parser_storage_authority\":false"),
                "parser evidence claims storage authority") &&
         Expect(Contains(result.sblr_envelope, "\"parser_evidence\":{"),
                "SBLR envelope missing parser evidence") &&
         Expect(Contains(result.sblr_envelope,
                         "\"full_declared_surface_assignment\":true"),
                "SBLR envelope missing full surface assignment flag");
}

bool ExpectDatatypeDescriptorPayload(
    std::string_view label,
    std::string_view payload,
    std::initializer_list<std::string_view> redacted_markers) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"datatype_descriptor_evidence\":{"),
               std::string(label) + " missing datatype descriptor evidence");
  ok &= Expect(Contains(payload,
                        "\"evidence_contract\":\"compatibility_datatype_descriptor_evidence.v1\""),
               std::string(label) + " missing datatype evidence contract");
  ok &= Expect(Contains(payload, "\"descriptor_resolution\":\"uuid_required\""),
               std::string(label) + " missing datatype UUID descriptor rule");
  ok &= Expect(Contains(payload, "\"datatype_reference_count\":1") ||
                   Contains(payload, "\"datatype_surface_matched\":true"),
               std::string(label) + " missing datatype surface match proof");
  ok &= Expect(Contains(payload, "\"catalog_descriptor_required\":true"),
               std::string(label) + " missing catalog descriptor requirement");
  ok &= Expect(Contains(payload, "\"wire_literal_cast_comparison_required\":true"),
               std::string(label) + " missing wire/literal cast proof requirement");
  ok &= Expect(Contains(payload, "\"collation_charset_profile_required\":true") ||
                   Contains(payload, "\"compatibility_datatype_profile_required\":true"),
               std::string(label) + " missing datatype profile requirement");
  ok &= Expect(Contains(payload, "\"generic_text_fallback_allowed\":false"),
               std::string(label) + " allowed generic text fallback");
  ok &= Expect(Contains(payload, "\"parser_storage_authority\":false"),
               std::string(label) + " claimed parser storage authority");
  ok &= Expect(Contains(payload, "\"parser_transaction_authority\":false"),
               std::string(label) + " claimed parser transaction authority");
  ok &= Expect(Contains(payload, "\"compatibility_sql_executed\":false"),
               std::string(label) + " claimed compatibility SQL execution");
  ok &= Expect(Contains(payload,
                        "\"exactness_status\":\"descriptor_surface_recorded_exactness_proof_pending\""),
               std::string(label) + " missing datatype exactness tracking status");
  ok &= Expect(Contains(payload, "\"enterprise_readiness\":\"not_enterprise_ready\""),
               std::string(label) + " must not claim enterprise readiness");
  for (const auto marker : redacted_markers) {
    ok &= Expect(!Contains(payload, marker),
                 std::string(label) + " leaked DDL sentinel marker: " +
                     std::string(marker));
  }
  return ok;
}

template <typename Result>
bool ExpectDirectDatatypeDescriptorEvidence(
    const Result& result,
    std::string_view label,
    std::initializer_list<std::string_view> redacted_markers) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " parse failed");
  ok &= ExpectDatatypeDescriptorPayload(
      std::string(label) + " parser evidence", result.parser_evidence_json,
      redacted_markers);
  ok &= ExpectDatatypeDescriptorPayload(
      std::string(label) + " SBLR envelope", result.sblr_envelope,
      redacted_markers);
  ok &= Expect(Contains(result.sblr_envelope,
                        "\"datatype_exactness_status\":\"surface_cataloged_exactness_proof_pending\""),
               std::string(label) + " lost datatype readiness blocker proof");
  return ok;
}

template <typename ParseFn>
bool ExpectRoute(ParseFn parse_fn,
                 std::string_view dialect,
                 std::string_view sql,
                 std::string_view operation_family,
                 std::string_view statement_kind,
                 bool parser_support_udr_route = false,
                 bool catalog_projection_only = false,
                 bool scratchbird_lifecycle_api = false,
                 bool fail_closed_refusal = false) {
  const auto result = parse_fn(sql);
  if (!Expect(result.ok, std::string("route parse failed for: ") +
                           std::string(sql))) return false;
  if (!Expect(result.operation_family == operation_family,
              std::string("operation family mismatch for: ") +
                  std::string(sql))) return false;
  if (!Expect(result.parser_support_udr_route == parser_support_udr_route,
              std::string("parser-support UDR route mismatch for: ") +
                  std::string(sql))) return false;
  if (!Expect(result.catalog_projection_only == catalog_projection_only,
              std::string("catalog projection route mismatch for: ") +
                  std::string(sql))) return false;
  if (!Expect(result.scratchbird_lifecycle_api == scratchbird_lifecycle_api,
              std::string("lifecycle route mismatch for: ") +
                  std::string(sql))) return false;
  if (!Expect(result.fail_closed_refusal == fail_closed_refusal,
              std::string("fail-closed route mismatch for: ") +
                  std::string(sql))) return false;
  if (!ExpectParserEvidence(result, dialect, statement_kind)) {
    std::cerr << "parser evidence mismatch for: " << sql
              << " expected statement kind: " << statement_kind << '\n';
    return false;
  }
  return true;
}

template <typename ParseFn>
bool ExpectUnsupportedDenied(ParseFn parse_fn,
                             std::string_view dialect,
                             std::string_view sql,
                             std::string_view operation_family,
                             std::string_view statement_kind,
                             std::string_view diagnostic_code) {
  const auto result = parse_fn(sql);
  if (!Expect(result.ok, std::string("unsupported-denied parse failed before routing for: ") +
                           std::string(sql))) return false;
  if (!Expect(result.operation_family == operation_family,
              std::string("unsupported-denied operation family mismatch for: ") +
                  std::string(sql))) return false;
  if (!Expect(!result.parser_support_udr_route,
              std::string("unsupported-denied route reached parser-support UDR for: ") +
                  std::string(sql))) return false;
  if (!Expect(!result.catalog_projection_only,
              std::string("unsupported-denied route became catalog projection for: ") +
                  std::string(sql))) return false;
  if (!Expect(!result.scratchbird_lifecycle_api,
              std::string("unsupported-denied route reached lifecycle API for: ") +
                  std::string(sql))) return false;
  if (!Expect(result.fail_closed_refusal,
              std::string("unsupported-denied route did not fail closed for: ") +
                  std::string(sql))) return false;
  if (!ExpectDiagnostic(result.message_vector_json, diagnostic_code,
                        std::string(dialect) + " unsupported-denied surface")) {
    return false;
  }
  if (!ExpectParserEvidence(result, dialect, statement_kind)) {
    std::cerr << "parser evidence mismatch for unsupported-denied surface: "
              << sql << " expected statement kind: " << statement_kind << '\n';
    return false;
  }
  return true;
}

template <typename ParseFn>
bool ExpectOwnUnsupported(ParseFn parse_fn,
                          std::string_view dialect,
                          std::string_view sql,
                          std::string_view diagnostic_code) {
  const auto result = parse_fn(sql);
  if (!Expect(!result.ok, std::string("unsupported route was admitted by ") +
                           std::string(dialect) + ": " +
                           std::string(sql))) return false;
  if (!ExpectDiagnostic(result.message_vector_json, diagnostic_code,
                        std::string(dialect) + " unsupported surface")) {
    return false;
  }
  return true;
}

bool CompatibilityWorkerSessionChecks(std::string_view label,
                              const scratchbird::parser::compatibility::DialectProfile& profile,
                              std::string_view expected_dialect) {
  bool close = false;
  const auto ping =
      scratchbird::parser::compatibility::HandleWorkerCommand("PING", profile, &close);
  if (!Expect(Contains(ping, "OK PONG " + std::string(expected_dialect)),
              std::string(label) + " worker ping mismatch")) return false;

  const auto wire = scratchbird::parser::compatibility::HandleWorkerCommand(
      "WIRE_TRANSCRIPT connect_auth_startup", profile, &close);
  if (!Expect(Contains(wire, "WIRE ") &&
                  Contains(wire, "\"parser_wire_owner\":true") &&
                  Contains(wire, "\"listener_boundary\":\"handoff_only\"") &&
                  Contains(wire, "\"auth_authority\":\"scratchbird_engine\"") &&
                  Contains(wire, "\"normalizes_secrets\":true") &&
                  Contains(wire, "\"compatibility_storage_authority\":false") &&
                  Contains(wire, "project/tests/reference_regression/" +
                                     std::string(expected_dialect) +
                                     "/wire_transcripts/connect_auth_startup/"),
              std::string(label) + " wire transcript mismatch")) return false;

  const auto resource = scratchbird::parser::compatibility::HandleWorkerCommand(
      "RESOURCE_LIMIT_REPORT", profile, &close);
  if (!Expect(Contains(resource, "RESOURCE ") &&
                  Contains(resource, "\"parse_timeout_ms\":30000") &&
                  Contains(resource, "\"cancellation_token_authority\":\"scratchbird_engine\"") &&
                  Contains(resource, "\"backpressure_policy\":\"fail_closed\"") &&
                  Contains(resource, "\"mga_transaction_authority\":\"scratchbird_engine\"") &&
                  Contains(resource, "\"parser_transaction_finality_authority\":false"),
              std::string(label) + " resource limit mismatch")) return false;

  const auto regression = scratchbird::parser::compatibility::HandleWorkerCommand(
      "REGRESSION_MANIFEST_REPORT", profile, &close);
  if (!Expect(Contains(regression, "REGRESSION ") &&
                  Contains(regression, "\"proof_location\":\"project/tests\"") &&
                  Contains(regression, "\"file_presence_is_completion\":false") &&
                  Contains(regression, "\"generated_only_completion\":false") &&
                  Contains(regression, "\"runtime_behavior_required\":true") &&
                  Contains(regression, "project/tests/reference_regression/" +
                                         std::string(expected_dialect) +
                                         "/management_package_abi/management_package_abi_manifest.csv") &&
                  Contains(regression, "project/tests/reference_regression/" +
                                         std::string(expected_dialect) +
                                         "/enterprise_completion/enterprise_completion_manifest.csv"),
              std::string(label) + " regression manifest mismatch")) return false;

  const auto invalid_wire = scratchbird::parser::compatibility::HandleWorkerCommand(
      "WIRE_TRANSCRIPT unsupported_family", profile, &close);
  if (!Expect(Contains(invalid_wire, "ERROR ") &&
                  Contains(invalid_wire, ".WORKER.WIRE_TRANSCRIPT_UNKNOWN") &&
                  Contains(invalid_wire, "\"runtime_policy\":\"fail_closed\""),
              std::string(label) + " invalid wire transcript did not fail closed")) {
    return false;
  }
  return true;
}

bool FirebirdChecks() {
  using namespace scratchbird::parser::firebird;
  if (!Expect(DatatypeSurfaces().size() == 8,
              "Firebird datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 8,
              "Firebird builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 7,
              "Firebird catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 7,
              "Firebird diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(FirebirdPackageIdentityJson(), "\"dialect\":\"firebird\""),
              "Firebird package identity missing dialect")) return false;
  if (!ExpectCommonEnvelope(ParseStatement("select 1"), "firebird",
                            "firebird.query.select")) return false;
  const auto evidence = ParseStatement(
      "select count(*) from enterprise_secret_rdb_relation where id = :id");
  if (!ExpectParserEvidence(evidence, "firebird", "SELECT")) return false;
  if (!Expect(!Contains(evidence.sblr_envelope, "enterprise_secret_rdb_relation") &&
                  !Contains(evidence.parser_evidence_json,
                            "enterprise_secret_rdb_relation"),
              "Firebird parser evidence leaked source text")) return false;
  if (!ExpectCommonEnvelope(ParseStatement("create database 'x'"), "firebird",
                            "firebird.emulated.database_lifecycle")) return false;
  const auto ddl_datatype = ParseStatement(
      "create table ddl_fb_sentinel_type_table (ddl_fb_col_int integer, ddl_fb_col_v varchar(40), ddl_fb_col_ts timestamp, ddl_fb_col_blob blob)");
  if (!ExpectDirectDatatypeDescriptorEvidence(
          ddl_datatype, "Firebird DDL datatype descriptor",
          {"ddl_fb_sentinel_type_table", "ddl_fb_col_int", "ddl_fb_col_v",
           "ddl_fb_col_ts", "ddl_fb_col_blob"})) {
    return false;
  }
  const auto file_surface = ParseStatement("backup database to '/tmp/a.fbk'");
  if (!Expect(file_surface.ok, "Firebird backup parse failed")) return false;
  if (!ExpectDiagnostic(file_surface.message_vector_json,
                        "FIREBIRD.EMULATION.NON_FILE_SURFACE",
                        "Firebird non-file surface")) return false;
  const auto replication_journal =
      ParseStatement("create journal archive 'scratchbird-firebird-archive'");
  if (!ExpectCommonEnvelope(replication_journal, "firebird",
                            "firebird.emulated.replication_journal")) {
    return false;
  }
  if (!Expect(replication_journal.parser_support_udr_route &&
                  !replication_journal.scratchbird_lifecycle_api &&
                  !replication_journal.real_firebird_file_effects &&
                  !replication_journal.reference_engine_sql_executed,
              "Firebird replication/journal route did not stay in parser-support UDR policy")) {
    return false;
  }
  if (!Expect(Contains(replication_journal.sblr_envelope,
                       "\"parser_support_udr_route\":true") &&
                  Contains(replication_journal.sblr_envelope,
                           "\"mapping_disposition\":\"parser_support_udr\"") &&
                  Contains(replication_journal.sblr_envelope,
                           "\"sblr_operation\":\"SBLR_COMPATIBILITY_FIREBIRD_REPLICATION_ROUTE\"") &&
                  Contains(replication_journal.message_vector_json,
                           "FIREBIRD.EMULATION.NON_FILE_SURFACE"),
              "Firebird replication/journal route evidence mismatch")) {
    return false;
  }
  const auto gbak_stdout =
      ParseStatement("gbak -b scratchbird-firebird.fdb stdout");
  if (!ExpectCommonEnvelope(gbak_stdout, "firebird",
                            "firebird.logical_stream.gbak_backup")) {
    return false;
  }
  if (!Expect(gbak_stdout.statement_family == "logical_stream_backup_restore" &&
                  !gbak_stdout.scratchbird_lifecycle_api &&
                  !gbak_stdout.real_firebird_file_effects &&
                  !gbak_stdout.reference_engine_sql_executed,
              "Firebird gbak stdout stream claimed forbidden authority")) {
    return false;
  }
  const auto gbak_stdout_parallel =
      ParseStatement("gbak -b scratchbird-firebird.fdb stdout -parallel 2");
  if (!ExpectCommonEnvelope(gbak_stdout_parallel, "firebird",
                            "firebird.logical_stream.gbak_backup")) {
    return false;
  }
  const auto gbak_stdin =
      ParseStatement("gbak -r stdin scratchbird-firebird.fdb");
  if (!ExpectCommonEnvelope(gbak_stdin, "firebird",
                            "firebird.logical_stream.gbak_restore")) {
    return false;
  }
  if (!Expect(gbak_stdin.statement_family == "logical_stream_backup_restore" &&
                  !gbak_stdin.scratchbird_lifecycle_api &&
                  !gbak_stdin.real_firebird_file_effects &&
                  !gbak_stdin.reference_engine_sql_executed,
              "Firebird gbak stdin stream claimed forbidden authority")) {
    return false;
  }
  const auto gbak_stdin_parallel =
      ParseStatement("gbak -r stdin scratchbird-firebird.fdb -parallel 2");
  if (!ExpectCommonEnvelope(gbak_stdin_parallel, "firebird",
                            "firebird.logical_stream.gbak_restore")) {
    return false;
  }
  const auto gbak_file_backup =
      ParseStatement("gbak -backup scratchbird-firebird.fdb scratchbird-firebird.fbk");
  if (!Expect(!gbak_file_backup.ok &&
                  !gbak_file_backup.scratchbird_lifecycle_api,
              "Firebird gbak file backup was admitted")) {
    return false;
  }
  if (!ExpectDiagnostic(gbak_file_backup.message_vector_json,
                        "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
                        "Firebird gbak file backup")) return false;
  const auto gbak_status_redirect_backup =
      ParseStatement("gbak -backup scratchbird-firebird.fdb scratchbird-firebird.fbk -y stdout");
  if (!Expect(!gbak_status_redirect_backup.ok &&
                  !gbak_status_redirect_backup.scratchbird_lifecycle_api,
              "Firebird gbak status redirect backup was admitted")) {
    return false;
  }
  if (!ExpectDiagnostic(gbak_status_redirect_backup.message_vector_json,
                        "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
                        "Firebird gbak status redirect backup")) return false;
  const auto gbak_file_restore =
      ParseStatement("gbak -restore scratchbird-firebird.fbk scratchbird-firebird.fdb");
  if (!Expect(!gbak_file_restore.ok &&
                  !gbak_file_restore.scratchbird_lifecycle_api,
              "Firebird gbak file restore was admitted")) {
    return false;
  }
  if (!ExpectDiagnostic(gbak_file_restore.message_vector_json,
                        "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
                        "Firebird gbak file restore")) return false;
  const auto gbak_status_redirect_restore =
      ParseStatement("gbak -restore -y stdin scratchbird-firebird.fbk scratchbird-firebird.fdb");
  if (!Expect(!gbak_status_redirect_restore.ok &&
                  !gbak_status_redirect_restore.scratchbird_lifecycle_api,
              "Firebird gbak status redirect restore was admitted")) {
    return false;
  }
  if (!ExpectDiagnostic(gbak_status_redirect_restore.message_vector_json,
                        "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
                        "Firebird gbak status redirect restore")) return false;
  const auto nbackup =
      ParseStatement("nbackup -backup 0 scratchbird-firebird.fdb scratchbird-firebird.nbk");
  if (!Expect(!nbackup.ok && !nbackup.scratchbird_lifecycle_api,
              "Firebird nbackup was admitted")) return false;
  if (!ExpectDiagnostic(nbackup.message_vector_json,
                        "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
                        "Firebird nbackup")) return false;
  const auto validate = ParseStatement("validate database");
  if (!Expect(!validate.ok && !validate.scratchbird_lifecycle_api,
              "Firebird VALIDATE was admitted as lifecycle authority")) return false;
  if (!ExpectDiagnostic(validate.message_vector_json,
                        "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
                        "Firebird VALIDATE")) return false;
  const auto repair = ParseStatement("repair database");
  if (!Expect(!repair.ok && !repair.scratchbird_lifecycle_api,
              "Firebird REPAIR was admitted as lifecycle authority")) return false;
  if (!ExpectDiagnostic(repair.message_vector_json,
                        "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
                        "Firebird REPAIR")) return false;
  const auto gfix = ParseStatement("gfix -mend database.fdb");
  if (!Expect(!gfix.ok && !gfix.scratchbird_lifecycle_api,
              "Firebird GFIX was admitted as lifecycle authority")) return false;
  if (!ExpectDiagnostic(gfix.message_vector_json,
                        "FIREBIRD.AUTHORITY.UNSUPPORTED_DENIED",
                        "Firebird GFIX")) return false;
  const auto rejected = ParseStatement("update rdb$relations set rdb$relation_name='X'");
  if (!Expect(!rejected.ok, "Firebird catalog mutation was accepted")) return false;
  if (!ExpectDiagnostic(rejected.message_vector_json,
                        "FIREBIRD.CATALOG_OVERLAY.READ_ONLY",
                        "Firebird catalog mutation")) return false;
  const auto literal_backtick = ParseStatement(
      "execute block as begin exception ex 'can`t lock any row in `qdistr`'; end");
  if (!Expect(literal_backtick.ok,
              "Firebird string literal was misclassified as MySQL syntax")) return false;
  if (!Expect(literal_backtick.operation_family == "firebird.psql.execute_block",
              "Firebird literal-marker execute block routed incorrectly")) return false;
  const auto literal_ipv6 = ParseStatement("connect 'inet6://[::1]/DYNAMIC_VALUE'");
  if (!ExpectCommonEnvelope(literal_ipv6, "firebird",
                            "firebird.isql.connect")) return false;
  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectOwnUnsupported(parse, "firebird",
                            "unrecognized_firebird_command",
                            "FIREBIRD.PARSE.INVALID_INPUT")) return false;
  return true;
}

bool MysqlChecks() {
  using namespace scratchbird::parser::mysql;
  if (!Expect(DatatypeSurfaces().size() == 10,
              "MySQL datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 10,
              "MySQL builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 8,
              "MySQL catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 11,
              "MySQL diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(MysqlPackageIdentityJson(), "\"function_api_rows\":123"),
              "MySQL package identity missing function inventory count")) return false;
  const auto tokens = LexTokens("select `Mixed Name`, @v, @@version # comment\n");
  bool saw_quoted = false;
  bool saw_variable = false;
  bool saw_comment = false;
  for (const auto& token : tokens) {
    saw_quoted = saw_quoted || token.kind == "quoted_identifier";
    saw_variable = saw_variable || token.kind == "parameter_or_variable";
    saw_comment = saw_comment || token.kind == "line_comment";
  }
  if (!Expect(saw_quoted && saw_variable && saw_comment,
              "MySQL lexer did not classify quoted identifiers, variables, and comments")) {
    return false;
  }
  if (!ExpectCommonEnvelope(ParseStatement("select json_extract(doc, '$.a') from t"),
                            "mysql", "mysql.query.select")) return false;
  const auto evidence = ParseStatement(
      "select json_extract(doc, '$.a') from enterprise_secret_table where id = ?");
  if (!ExpectParserEvidence(evidence, "mysql", "SELECT")) return false;
  if (!Expect(!Contains(evidence.sblr_envelope, "enterprise_secret_table") &&
                  !Contains(evidence.parser_evidence_json, "enterprise_secret_table"),
              "MySQL parser evidence leaked source text")) return false;
  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "mysql", "select 1", "mysql.query.select",
                   "SELECT")) return false;
  if (!ExpectRoute(parse, "mysql", "with q as (select 1) select * from q",
                   "mysql.query.with", "WITH")) return false;
  if (!ExpectRoute(parse, "mysql", "insert into t values (1)",
                   "mysql.dml.insert", "INSERT")) return false;
  if (!ExpectRoute(parse, "mysql", "replace into t values (1)",
                   "mysql.dml.replace", "REPLACE")) return false;
  if (!ExpectRoute(parse, "mysql", "update t set a = 1",
                   "mysql.dml.update", "UPDATE")) return false;
  if (!ExpectRoute(parse, "mysql", "delete from t",
                   "mysql.dml.delete", "DELETE")) return false;
  if (!ExpectRoute(parse, "mysql", "create table t (id int)",
                   "mysql.ddl.create", "CREATE_TABLE")) return false;
  const auto ddl_datatype = ParseStatement(
      "create table ddl_mysql_sentinel_type_table (ddl_mysql_col_int int, ddl_mysql_col_v varchar(40), ddl_mysql_col_dec decimal(10,2), ddl_mysql_col_ts timestamp, ddl_mysql_col_json json)");
  if (!ExpectDirectDatatypeDescriptorEvidence(
          ddl_datatype, "MySQL DDL datatype descriptor",
          {"ddl_mysql_sentinel_type_table", "ddl_mysql_col_int",
           "ddl_mysql_col_v", "ddl_mysql_col_dec", "ddl_mysql_col_ts",
           "ddl_mysql_col_json"})) {
    return false;
  }
  if (!ExpectRoute(parse, "mysql", "alter table t add column name text",
                   "mysql.ddl.alter", "ALTER_TABLE")) return false;
  if (!ExpectRoute(parse, "mysql", "drop table t",
                   "mysql.ddl.drop", "DROP_TABLE")) return false;
  if (!ExpectRoute(parse, "mysql", "truncate table t",
                   "mysql.ddl.truncate", "TRUNCATE")) return false;
  if (!ExpectRoute(parse, "mysql", "use app",
                   "mysql.session.use_database", "USE")) return false;
  if (!ExpectRoute(parse, "mysql", "set sql_mode = 'ANSI'",
                   "mysql.session.set", "SET")) return false;
  if (!ExpectRoute(parse, "mysql", "prepare s from 'select 1'",
                   "mysql.prepared.prepare", "PREPARE")) return false;
  if (!ExpectRoute(parse, "mysql", "execute s",
                   "mysql.prepared.execute", "EXECUTE")) return false;
  if (!ExpectRoute(parse, "mysql", "deallocate prepare s",
                   "mysql.prepared.deallocate", "DEALLOCATE")) return false;
  if (!ExpectRoute(parse, "mysql", "lock tables t write",
                   "mysql.locking.lock_tables", "LOCK")) return false;
  if (!ExpectRoute(parse, "mysql", "unlock tables",
                   "mysql.locking.unlock_tables", "UNLOCK")) return false;
  if (!ExpectRoute(parse, "mysql", "start transaction",
                   "mysql.transaction.start", "START_TRANSACTION")) return false;
  if (!ExpectRoute(parse, "mysql", "begin",
                   "mysql.transaction.begin", "BEGIN")) return false;
  if (!ExpectRoute(parse, "mysql", "commit",
                   "mysql.transaction.commit", "COMMIT")) return false;
  if (!ExpectRoute(parse, "mysql", "rollback",
                   "mysql.transaction.rollback", "ROLLBACK")) return false;
  if (!ExpectRoute(parse, "mysql", "savepoint s1",
                   "mysql.transaction.savepoint", "SAVEPOINT")) return false;
  if (!ExpectRoute(parse, "mysql", "release savepoint s1",
                   "mysql.transaction.release_savepoint", "RELEASE")) return false;
  if (!ExpectRoute(parse, "mysql", "show tables",
                   "mysql.catalog_overlay.show", "SHOW", false, true)) return false;
  if (!ExpectRoute(parse, "mysql", "describe t",
                   "mysql.catalog_overlay.describe", "DESCRIBE", false, true)) return false;
  if (!ExpectRoute(parse, "mysql", "explain select * from t",
                   "mysql.optimizer.explain", "EXPLAIN", false, true)) return false;
  if (!ExpectRoute(parse, "mysql", "create database app",
                   "mysql.lifecycle.create_database", "CREATE_DATABASE",
                   false, false, true)) return false;
  if (!ExpectRoute(parse, "mysql", "drop database app",
                   "mysql.lifecycle.drop_database", "DROP_DATABASE",
                   false, false, true)) return false;
  if (!ExpectRoute(parse, "mysql", "create user 'b'@'localhost'",
                   "mysql.security.create_user", "CREATE_USER", true)) return false;
  if (!ExpectRoute(parse, "mysql", "alter user 'b'@'localhost' account lock",
                   "mysql.security.alter_user", "ALTER_USER", true)) return false;
  if (!ExpectRoute(parse, "mysql", "drop user 'b'@'localhost'",
                   "mysql.security.drop_user", "DROP_USER", true)) return false;
  if (!ExpectRoute(parse, "mysql", "create role app_reader",
                   "mysql.security.create_role", "CREATE_ROLE", true)) return false;
  if (!ExpectRoute(parse, "mysql", "drop role app_reader",
                   "mysql.security.drop_role", "DROP_ROLE", true)) return false;
  if (!ExpectRoute(parse, "mysql", "grant select on app.* to app_reader",
                   "mysql.security.grant", "GRANT", true)) return false;
  if (!ExpectRoute(parse, "mysql", "revoke select on app.* from app_reader",
                   "mysql.security.revoke", "REVOKE", true)) return false;
  if (!ExpectRoute(parse, "mysql", "create event ev on schedule every 1 day do select 1",
                   "mysql.routine.event.create", "CREATE_EVENT", true)) return false;
  if (!ExpectRoute(parse, "mysql", "create trigger tr before insert on t for each row set @x = 1",
                   "mysql.routine.trigger.create", "CREATE_TRIGGER", true)) return false;
  if (!ExpectRoute(parse, "mysql", "create procedure p() select 1",
                   "mysql.routine.procedure.create", "CREATE_PROCEDURE", true)) return false;
  if (!ExpectRoute(parse, "mysql", "create function f() returns int return 1",
                   "mysql.routine.function.create", "CREATE_FUNCTION", true)) return false;
  if (!ExpectRoute(parse, "mysql", "call p()",
                   "mysql.routine.call", "CALL", true)) return false;
  if (!ExpectRoute(parse, "mysql", "change replication source to source_host='h'",
                   "mysql.replication.change_source", "CHANGE", true)) return false;
  if (!ExpectRoute(parse, "mysql", "change master to master_host='h'",
                   "mysql.replication.change_master_legacy", "CHANGE", true)) return false;
  if (!ExpectRoute(parse, "mysql", "start replica",
                   "mysql.replication.start_replica", "START", true)) return false;
  if (!ExpectRoute(parse, "mysql", "stop replica",
                   "mysql.replication.stop_replica", "STOP", true)) return false;
  if (!ExpectRoute(parse, "mysql", "reset replica",
                   "mysql.replication.reset_replica", "RESET", true)) return false;
  if (!ExpectRoute(parse, "mysql", "show replica status",
                   "mysql.replication.show_replica_status", "SHOW", true)) return false;
  if (!ExpectRoute(parse, "mysql", "show slave status",
                   "mysql.replication.show_slave_status_legacy", "SHOW", true)) return false;
  if (!ExpectRoute(parse, "mysql", "purge binary logs to 'bin.000010'",
                   "mysql.replication.purge_binary_logs", "PURGE", true)) return false;
  if (!ExpectRoute(parse, "mysql", "reset binary logs and gtids",
                   "mysql.replication.reset_binary_logs", "RESET", true)) return false;
  if (!ExpectRoute(parse, "mysql",
                   "load data local infile 'client.csv' into table t",
                   "mysql.bulk_io.load_data_local_infile", "LOAD_DATA", true)) return false;
  if (!ExpectRoute(parse, "mysql",
                   "LOAD DATA LOCAL INFILE 'client.tsv' INTO TABLE t FIELDS TERMINATED BY '\t'",
                   "mysql.bulk_io.load_data_local_infile", "LOAD_DATA", true)) return false;
  if (!ExpectRoute(parse, "mysql",
                   "load data low_priority local infile 'client.csv' into table t",
                   "mysql.bulk_io.load_data_local_infile", "LOAD_DATA", true)) return false;
  if (!ExpectRoute(parse, "mysql",
                   "load data concurrent local infile 'client.csv' into table t",
                   "mysql.bulk_io.load_data_local_infile", "LOAD_DATA", true)) return false;
  if (!ExpectUnsupportedDenied(parse, "mysql", "analyze table t1",
                               "mysql.maintenance.analyze_table", "ANALYZE",
                               "MYSQL.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "mysql", "optimize table t1",
                               "mysql.maintenance.optimize_table", "OPTIMIZE",
                               "MYSQL.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "mysql", "check table t1",
                               "mysql.maintenance.check_table", "CHECK",
                               "MYSQL.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "mysql", "repair table t1",
                               "mysql.maintenance.repair_table", "REPAIR",
                               "MYSQL.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "mysql", "flush tables with read lock",
                               "mysql.maintenance.flush", "FLUSH",
                               "MYSQL.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectRoute(parse, "mysql", "xa start 'xid'",
                   "mysql.transaction.xa", "XA",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mysql", "select * from t into outfile '/tmp/x'",
                   "mysql.bulk_io.select_into_outfile", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mysql", "select * from t into dumpfile '/tmp/x'",
                   "mysql.bulk_io.select_into_dumpfile", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mysql", "select load_file('/tmp/x')",
                   "mysql.bulk_io.load_file", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mysql", "do load_file('/tmp/x')",
                   "mysql.bulk_io.load_file", "DO",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mysql", "insert into t values (load_file('/tmp/x'))",
                   "mysql.bulk_io.load_file", "INSERT",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mysql",
                   "load data low_priority infile '/tmp/x.csv' into table t",
                   "mysql.bulk_io.load_data_infile", "LOAD_DATA",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mysql",
                   "load data concurrent infile '/tmp/x.csv' into table t",
                   "mysql.bulk_io.load_data_infile", "LOAD_DATA",
                   false, false, false, true)) return false;
  if (!ExpectOwnUnsupported(parse, "mysql",
                            "load data locality infile 'client.csv' into table t",
                            "MYSQL.PARSE.UNSUPPORTED_SURFACE")) return false;
  if (!ExpectRoute(parse, "mysql", "select 'load data local infile client.csv'",
                   "mysql.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "mysql", "select 'select load_file(''/tmp/x'')'",
                   "mysql.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "mysql", "select 'select * from t into outfile ''/tmp/x'''",
                   "mysql.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "mysql", "select load_file_name from t",
                   "mysql.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "mysql", "select into_outfile from t",
                   "mysql.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "mysql", "select into_dumpfile from t",
                   "mysql.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "mysql", "create table load_file_name (id int)",
                   "mysql.ddl.create", "CREATE_TABLE")) return false;
  if (!ExpectRoute(parse, "mysql", "create tablespace ts add datafile 'x.ibd'",
                   "mysql.storage.tablespace.create", "CREATE_TABLESPACE",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mysql", "alter tablespace ts rename to ts2",
                   "mysql.storage.tablespace.alter", "ALTER_TABLESPACE",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mysql", "drop tablespace ts",
                   "mysql.storage.tablespace.drop", "DROP_TABLESPACE",
                   false, false, false, true)) return false;
  if (!CompatibilityWorkerSessionChecks("MySQL", Profile(), "mysql")) return false;
  if (!ExpectCommonEnvelope(ParseStatement("create database app"),
                            "mysql", "mysql.lifecycle.create_database")) return false;
  const auto load_data = ParseStatement("load data infile '/tmp/x.csv' into table t");
  if (!Expect(load_data.ok && load_data.fail_closed_refusal,
              "MySQL LOAD DATA did not produce fail-closed refusal")) return false;
  if (!ExpectDiagnostic(load_data.message_vector_json,
                        "MYSQL.AUTHORITY.FILE_IO_DENIED",
                        "MySQL LOAD DATA")) return false;
  const auto load_file = ParseStatement("select load_file('/tmp/x')");
  if (!Expect(load_file.ok && load_file.fail_closed_refusal,
              "MySQL LOAD_FILE did not produce fail-closed refusal")) return false;
  if (!ExpectDiagnostic(load_file.message_vector_json,
                        "MYSQL.AUTHORITY.FILE_IO_DENIED",
                        "MySQL LOAD_FILE")) return false;
  const auto plugin = ParseStatement("install plugin audit soname 'audit.so'");
  if (!Expect(plugin.ok && plugin.fail_closed_refusal,
              "MySQL INSTALL PLUGIN did not fail closed")) return false;
  if (!ExpectDiagnostic(plugin.message_vector_json,
                        "MYSQL.AUTHORITY.PLUGIN_DENIED",
                        "MySQL plugin")) return false;
  const auto user = ParseStatement("create user 'a'@'localhost' identified by 'x'");
  if (!Expect(user.ok && user.parser_support_udr_route,
              "MySQL CREATE USER did not route to parser-support UDR")) return false;
  const auto catalog_write = ParseStatement(
      "update information_schema.tables set table_name='x'");
  if (!Expect(!catalog_write.ok,
              "MySQL information_schema mutation was accepted")) return false;
  if (!ExpectDiagnostic(catalog_write.message_vector_json,
                        "MYSQL.CATALOG_OVERLAY.READ_ONLY",
                        "MySQL catalog mutation")) return false;
  if (!ExpectCommonEnvelope(ParseStatement(
                                "select 'copy public.t from stdin, 1::integer, rdb$relations'"),
                            "mysql", "mysql.query.select")) return false;
  if (!ExpectOwnUnsupported(parse, "mysql",
                            "unsupported_mysql_command",
                            "MYSQL.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool PostgresqlChecks() {
  using namespace scratchbird::parser::postgresql;
  if (!Expect(DatatypeSurfaces().size() == 12,
              "PostgreSQL datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 13,
              "PostgreSQL builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 9,
              "PostgreSQL catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 12,
              "PostgreSQL diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(PostgresqlPackageIdentityJson(), "\"function_api_rows\":2406"),
              "PostgreSQL package identity missing function inventory count")) return false;
  const auto tokens = LexTokens("select \"Mixed Name\", $1 from pg_catalog.pg_class -- c\n");
  bool saw_quoted = false;
  bool saw_parameter = false;
  bool saw_comment = false;
  for (const auto& token : tokens) {
    saw_quoted = saw_quoted || token.kind == "quoted_identifier";
    saw_parameter = saw_parameter || token.kind == "parameter_or_variable";
    saw_comment = saw_comment || token.kind == "line_comment";
  }
  if (!Expect(saw_quoted && saw_parameter && saw_comment,
              "PostgreSQL lexer did not classify quoted identifiers, parameters, and comments")) {
    return false;
  }
  if (!ExpectCommonEnvelope(ParseStatement("select count(*) from public.t"),
                            "postgresql", "postgresql.query.select")) return false;
  const auto evidence = ParseStatement(
      "select count(*) from enterprise_secret_schema.enterprise_secret_table where id = $1");
  if (!ExpectParserEvidence(evidence, "postgresql", "SELECT")) return false;
  if (!Expect(!Contains(evidence.sblr_envelope, "enterprise_secret_table") &&
                  !Contains(evidence.parser_evidence_json, "enterprise_secret_table"),
              "PostgreSQL parser evidence leaked source text")) return false;
  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "postgresql", "select 1",
                   "postgresql.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "postgresql", "with q as (select 1) select * from q",
                   "postgresql.query.with", "WITH")) return false;
  if (!ExpectRoute(parse, "postgresql", "insert into t values (1)",
                   "postgresql.dml.insert", "INSERT")) return false;
  if (!ExpectRoute(parse, "postgresql", "update t set a = 1",
                   "postgresql.dml.update", "UPDATE")) return false;
  if (!ExpectRoute(parse, "postgresql", "delete from t",
                   "postgresql.dml.delete", "DELETE")) return false;
  if (!ExpectRoute(parse, "postgresql", "merge into t using s on t.id=s.id when matched then update set a=s.a",
                   "postgresql.dml.merge", "MERGE")) return false;
  if (!ExpectRoute(parse, "postgresql", "create table t (id integer)",
                   "postgresql.ddl.create", "CREATE_TABLE")) return false;
  const auto ddl_datatype = ParseStatement(
      "create table ddl_pg_sentinel_type_table (ddl_pg_col_int integer, ddl_pg_col_text text, ddl_pg_col_num numeric(10,2), ddl_pg_col_tz timestamptz, ddl_pg_col_json jsonb, ddl_pg_col_uuid uuid)");
  if (!ExpectDirectDatatypeDescriptorEvidence(
          ddl_datatype, "PostgreSQL DDL datatype descriptor",
          {"ddl_pg_sentinel_type_table", "ddl_pg_col_int",
           "ddl_pg_col_text", "ddl_pg_col_num", "ddl_pg_col_tz",
           "ddl_pg_col_json", "ddl_pg_col_uuid"})) {
    return false;
  }
  if (!ExpectRoute(parse, "postgresql", "alter table t add column name text",
                   "postgresql.ddl.alter", "ALTER_TABLE")) return false;
  if (!ExpectRoute(parse, "postgresql", "drop table t",
                   "postgresql.ddl.drop", "DROP_TABLE")) return false;
  if (!ExpectRoute(parse, "postgresql", "truncate table t",
                   "postgresql.ddl.truncate", "TRUNCATE")) return false;
  if (!ExpectRoute(parse, "postgresql", "show search_path",
                   "postgresql.catalog_overlay.show", "SHOW", false, true)) return false;
  if (!ExpectRoute(parse, "postgresql", "explain select * from t",
                   "postgresql.optimizer.explain", "EXPLAIN", false, true)) return false;
  if (!ExpectRoute(parse, "postgresql", "set work_mem = '64MB'",
                   "postgresql.session.set", "SET")) return false;
  if (!ExpectRoute(parse, "postgresql", "begin",
                   "postgresql.transaction.begin", "BEGIN")) return false;
  if (!ExpectRoute(parse, "postgresql", "start transaction",
                   "postgresql.transaction.start", "START_TRANSACTION")) return false;
  if (!ExpectRoute(parse, "postgresql", "commit",
                   "postgresql.transaction.commit", "COMMIT")) return false;
  if (!ExpectRoute(parse, "postgresql", "rollback",
                   "postgresql.transaction.rollback", "ROLLBACK")) return false;
  if (!ExpectRoute(parse, "postgresql", "savepoint s1",
                   "postgresql.transaction.savepoint", "SAVEPOINT")) return false;
  if (!ExpectRoute(parse, "postgresql", "release savepoint s1",
                   "postgresql.transaction.release_savepoint", "RELEASE")) return false;
  if (!ExpectRoute(parse, "postgresql", "declare c cursor for select 1",
                   "postgresql.cursor.declare", "DECLARE")) return false;
  if (!ExpectRoute(parse, "postgresql", "fetch next from c",
                   "postgresql.cursor.fetch", "FETCH")) return false;
  if (!ExpectRoute(parse, "postgresql", "copy public.t from stdin",
                   "postgresql.logical_stream.copy_from_stdin", "COPY", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "copy public.t to stdout",
                   "postgresql.logical_stream.copy_to_stdout", "COPY", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "copy public.t to '/tmp/x.csv'",
                   "postgresql.bulk_io.copy_to_file", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "postgresql", "copy public.t from '/tmp/x.csv'",
                   "postgresql.bulk_io.copy_from_file", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "postgresql", "copy public.t to :'filename' csv",
                   "postgresql.bulk_io.copy_to_file", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "postgresql", "copy public.t from :'filename' csv",
                   "postgresql.bulk_io.copy_from_file", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "postgresql", "copy public.t to 'stdout'",
                   "postgresql.bulk_io.copy_to_file", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "postgresql", "copy public.t from 'stdin'",
                   "postgresql.bulk_io.copy_from_file", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "postgresql", "alter table public.t rename to stdout",
                   "postgresql.ddl.alter", "ALTER_TABLE")) return false;
  if (!ExpectRoute(parse, "postgresql", "select * from stdin",
                   "postgresql.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_import('/tmp/x')",
                   "postgresql.large_object.lo_import", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("select lo_import('/tmp/x')").message_vector_json,
                        "POSTGRESQL.AUTHORITY.FILE_IO_DENIED",
                        "PostgreSQL lo_import server file authority")) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_export(1, '/tmp/x')",
                   "postgresql.large_object.lo_export", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("select lo_export(1, '/tmp/x')").message_vector_json,
                        "POSTGRESQL.AUTHORITY.FILE_IO_DENIED",
                        "PostgreSQL lo_export server file authority")) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_open(1, 0)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_create(0)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_creat(-1)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_close(1)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_read(1, 8192)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_write(1, '\\x00'::bytea)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select loread(1, 8192)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lowrite(1, '\\x00'::bytea)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_lseek(1, 0, 0)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_lseek64(1, 0, 0)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_tell(1)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_tell64(1)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_truncate(1, 0)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_truncate64(1, 0)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_from_bytea(0, '\\x00'::bytea)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_get(1)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_put(1, 0, '\\x00'::bytea)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_unlink(1)",
                   "postgresql.large_object.api", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_import_name from t",
                   "postgresql.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_export_name from t",
                   "postgresql.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "postgresql", "create table lo_import_name (id integer)",
                   "postgresql.ddl.create", "CREATE_TABLE")) return false;
  if (!ExpectRoute(parse, "postgresql", "select 'lo_import(''/tmp/x'')'",
                   "postgresql.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "postgresql", "select 'lo_export(1, ''/tmp/x'')'",
                   "postgresql.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "postgresql", "select 1 -- lo_import('/tmp/x')",
                   "postgresql.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "postgresql", "select lo_value from t",
                   "postgresql.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "postgresql", "create database app",
                   "postgresql.lifecycle.create_database", "CREATE_DATABASE",
                   false, false, true)) return false;
  if (!ExpectRoute(parse, "postgresql", "drop database app",
                   "postgresql.lifecycle.drop_database", "DROP_DATABASE",
                   false, false, true)) return false;
  if (!ExpectRoute(parse, "postgresql", "create role app_reader",
                   "postgresql.security.create_role", "CREATE_ROLE", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "alter role app_reader login",
                   "postgresql.security.alter_role", "ALTER_ROLE", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "drop role app_reader",
                   "postgresql.security.drop_role", "DROP_ROLE", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "create user app_user",
                   "postgresql.security.create_user", "CREATE_USER", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "grant select on t to app_reader",
                   "postgresql.security.grant", "GRANT", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "revoke select on t from app_reader",
                   "postgresql.security.revoke", "REVOKE", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "create policy p on t using (true)",
                   "postgresql.security.row_policy.create", "CREATE_POLICY", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "create function f() returns integer language sql as 'select 1'",
                   "postgresql.routine.function.create", "CREATE_FUNCTION", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "create procedure p() language sql as 'select 1'",
                   "postgresql.routine.procedure.create", "CREATE_PROCEDURE", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "create trigger tr before insert on t execute function f()",
                   "postgresql.routine.trigger.create", "CREATE_TRIGGER", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "create rule r as on insert to t do nothing",
                   "postgresql.routine.rule.create", "CREATE_RULE", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "do 'begin null; end'",
                   "postgresql.routine.do_block", "DO", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "call p()",
                   "postgresql.routine.call", "CALL", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "listen app_channel",
                   "postgresql.events.listen", "LISTEN", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "notify app_channel, 'payload'",
                   "postgresql.events.notify", "NOTIFY", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "unlisten notify_async2",
                   "postgresql.events.unlisten", "UNLISTEN", true)) return false;
  if (!ExpectUnsupportedDenied(parse, "postgresql", "vacuum t",
                               "postgresql.maintenance.vacuum", "VACUUM",
                               "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "postgresql", "analyze t",
                               "postgresql.maintenance.analyze", "ANALYZE",
                               "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "postgresql", "reindex table t",
                               "postgresql.maintenance.reindex", "REINDEX",
                               "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "postgresql", "cluster t using t_pkey",
                               "postgresql.maintenance.cluster", "CLUSTER",
                               "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "postgresql", "refresh materialized view mv",
                               "postgresql.maintenance.refresh_materialized_view",
                               "REFRESH",
                               "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectRoute(parse, "postgresql", "reset client_min_messages",
                   "postgresql.session.reset", "RESET")) return false;
  if (!ExpectRoute(parse, "postgresql", "discard sequences",
                   "postgresql.session.discard", "DISCARD")) return false;
  if (!ExpectRoute(parse, "postgresql", "prepare q(int) as select $1",
                   "postgresql.prepared.prepare", "PREPARE")) return false;
  if (!ExpectRoute(parse, "postgresql", "execute q(1)",
                   "postgresql.prepared.execute", "EXECUTE")) return false;
  if (!ExpectRoute(parse, "postgresql", "deallocate q",
                   "postgresql.prepared.deallocate", "DEALLOCATE")) return false;
  if (!ExpectRoute(parse, "postgresql", "move next from c",
                   "postgresql.cursor.move", "MOVE")) return false;
  if (!ExpectRoute(parse, "postgresql", "close c",
                   "postgresql.cursor.close", "CLOSE")) return false;
  if (!ExpectRoute(parse, "postgresql", "lock table t in access share mode",
                   "postgresql.locking.lock_table", "LOCK")) return false;
  if (!ExpectRoute(parse, "postgresql", "security label on table t is 'x'",
                   "postgresql.security.security_label", "SECURITY", true)) return false;
  if (!ExpectUnsupportedDenied(parse, "postgresql", "checkpoint",
                               "postgresql.system.checkpoint", "CHECKPOINT",
                               "POSTGRESQL.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectRoute(parse, "postgresql", "prepare transaction 'x'",
                   "postgresql.transaction.prepare_transaction", "PREPARE",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "postgresql", "comment on table attmp is 'table comment'",
                   "postgresql.ddl.comment", "COMMENT",
                   false, false, false, false)) return false;
  if (!ExpectRoute(parse, "postgresql", "create extension pgcrypto",
                   "postgresql.extension.create", "CREATE_EXTENSION", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "alter extension pgcrypto update",
                   "postgresql.extension.alter", "ALTER_EXTENSION", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "drop extension pgcrypto",
                   "postgresql.extension.drop", "DROP_EXTENSION", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "create foreign data wrapper scratchbird_fdw",
                   "postgresql.connector.fdw.create", "CREATE_FOREIGN",
                   true)) return false;
  if (!ExpectRoute(parse, "postgresql", "create server scratchbird foreign data wrapper scratchbird_fdw",
                   "postgresql.connector.server.create", "CREATE_SERVER",
                   true)) return false;
  if (!ExpectRoute(parse, "postgresql", "create user mapping for app_user server scratchbird",
                   "postgresql.connector.user_mapping.create", "CREATE_USER",
                   true)) return false;
  if (!ExpectRoute(parse, "postgresql", "import foreign schema public from server scratchbird into public",
                   "postgresql.connector.import_foreign_schema", "IMPORT",
                   true)) return false;
  if (!ExpectRoute(parse, "postgresql", "create foreign table ft (id integer) server scratchbird",
                   "postgresql.connector.foreign_table.create", "CREATE_FOREIGN",
                   true)) return false;
  if (!ExpectRoute(parse, "postgresql", "create publication pub for table t",
                   "postgresql.replication.publication.create", "CREATE_PUBLICATION", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "create subscription sub connection 'host=x' publication pub",
                   "postgresql.replication.subscription.create", "CREATE_SUBSCRIPTION", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "alter subscription sub disable",
                   "postgresql.replication.subscription.alter", "ALTER_SUBSCRIPTION", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "drop subscription sub",
                   "postgresql.replication.subscription.drop", "DROP_SUBSCRIPTION", true)) return false;
  if (!ExpectRoute(parse, "postgresql", "create tablespace ts location '/tmp/ts'",
                   "postgresql.storage.tablespace.create", "CREATE_TABLESPACE",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "postgresql", "alter tablespace ts set (random_page_cost = 1.0)",
                   "postgresql.storage.tablespace.alter", "ALTER_TABLESPACE",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "postgresql", "drop tablespace ts",
                   "postgresql.storage.tablespace.drop", "DROP_TABLESPACE",
                   false, false, false, true)) return false;
  if (!CompatibilityWorkerSessionChecks("PostgreSQL", Profile(), "postgresql")) return false;
  const auto copy_stdin = ParseStatement("copy public.t from stdin");
  if (!ExpectCommonEnvelope(copy_stdin, "postgresql",
                            "postgresql.logical_stream.copy_from_stdin")) {
    return false;
  }
  if (!Expect(copy_stdin.statement_family == "logical_stream_backup_restore" &&
                  copy_stdin.parser_support_udr_route &&
                  !copy_stdin.scratchbird_lifecycle_api &&
                  !copy_stdin.real_reference_file_effects &&
                  !copy_stdin.reference_engine_sql_executed &&
                  !copy_stdin.fail_closed_refusal,
              "PostgreSQL COPY FROM STDIN stream claimed forbidden authority")) {
    return false;
  }
  const auto copy_stdout = ParseStatement("copy public.t to stdout");
  if (!ExpectCommonEnvelope(copy_stdout, "postgresql",
                            "postgresql.logical_stream.copy_to_stdout")) {
    return false;
  }
  if (!Expect(copy_stdout.statement_family == "logical_stream_backup_restore" &&
                  copy_stdout.parser_support_udr_route &&
                  !copy_stdout.scratchbird_lifecycle_api &&
                  !copy_stdout.real_reference_file_effects &&
                  !copy_stdout.reference_engine_sql_executed &&
                  !copy_stdout.fail_closed_refusal,
              "PostgreSQL COPY TO STDOUT stream claimed forbidden authority")) {
    return false;
  }
  const auto copy_to_file = ParseStatement("copy public.t to '/tmp/x.csv'");
  if (!Expect(copy_to_file.ok && copy_to_file.fail_closed_refusal,
              "PostgreSQL COPY TO file did not fail closed")) return false;
  if (!ExpectDiagnostic(copy_to_file.message_vector_json,
                        "POSTGRESQL.AUTHORITY.FILE_IO_DENIED",
                        "PostgreSQL COPY TO file")) return false;
  const auto copy_from_file = ParseStatement("copy public.t from '/tmp/x.csv'");
  if (!Expect(copy_from_file.ok && copy_from_file.fail_closed_refusal,
              "PostgreSQL COPY FROM file did not fail closed")) return false;
  if (!ExpectDiagnostic(copy_from_file.message_vector_json,
                        "POSTGRESQL.AUTHORITY.FILE_IO_DENIED",
                        "PostgreSQL COPY FROM file")) return false;
  const auto copy_to_quoted_stdout = ParseStatement("copy public.t to 'stdout'");
  if (!Expect(copy_to_quoted_stdout.ok && copy_to_quoted_stdout.fail_closed_refusal,
              "PostgreSQL COPY TO quoted stdout did not fail closed")) return false;
  if (!ExpectDiagnostic(copy_to_quoted_stdout.message_vector_json,
                        "POSTGRESQL.AUTHORITY.FILE_IO_DENIED",
                        "PostgreSQL COPY TO quoted stdout")) return false;
  const auto copy_from_quoted_stdin = ParseStatement("copy public.t from 'stdin'");
  if (!Expect(copy_from_quoted_stdin.ok &&
                  copy_from_quoted_stdin.fail_closed_refusal,
              "PostgreSQL COPY FROM quoted stdin did not fail closed")) return false;
  if (!ExpectDiagnostic(copy_from_quoted_stdin.message_vector_json,
                        "POSTGRESQL.AUTHORITY.FILE_IO_DENIED",
                        "PostgreSQL COPY FROM quoted stdin")) return false;
  const auto copy_program = ParseStatement("copy public.t from program 'cat /etc/passwd'");
  if (!Expect(copy_program.ok && copy_program.fail_closed_refusal,
              "PostgreSQL COPY PROGRAM did not fail closed")) return false;
  if (!ExpectDiagnostic(copy_program.message_vector_json,
                        "POSTGRESQL.AUTHORITY.PROGRAM_DENIED",
                        "PostgreSQL COPY PROGRAM")) return false;
  const auto extension = ParseStatement("create extension pgcrypto");
  if (!Expect(extension.ok && extension.parser_support_udr_route,
              "PostgreSQL CREATE EXTENSION did not route to parser-support UDR")) return false;
  const auto fdw = ParseStatement("create foreign data wrapper scratchbird_fdw");
  if (!Expect(fdw.ok && fdw.parser_support_udr_route,
              "PostgreSQL FDW did not route to parser-support UDR")) return false;
  if (!ExpectDiagnostic(fdw.message_vector_json,
                        "POSTGRESQL.EMULATION.CONNECTOR_ROUTE",
                        "PostgreSQL FDW")) return false;
  const auto alter_system = ParseStatement("alter system set work_mem='64MB'");
  if (!Expect(alter_system.ok && alter_system.fail_closed_refusal,
              "PostgreSQL ALTER SYSTEM did not fail closed")) return false;
  if (!ExpectDiagnostic(alter_system.message_vector_json,
                        "POSTGRESQL.AUTHORITY.SYSTEM_DENIED",
                        "PostgreSQL ALTER SYSTEM")) return false;
  const auto catalog_write = ParseStatement(
      "delete from pg_catalog.pg_class where relname='x'");
  if (!Expect(!catalog_write.ok,
              "PostgreSQL pg_catalog mutation was accepted")) return false;
  if (!ExpectDiagnostic(catalog_write.message_vector_json,
                        "POSTGRESQL.CATALOG_OVERLAY.READ_ONLY",
                        "PostgreSQL catalog mutation")) return false;
  if (!ExpectCommonEnvelope(ParseStatement(
                                "select 'show tables, load_file(''/tmp/x''), `mysql_name`, rdb$relations'"),
                            "postgresql", "postgresql.query.select")) return false;
  if (!ExpectOwnUnsupported(parse, "postgresql",
                            "unsupported_postgresql_command",
                            "POSTGRESQL.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool SqliteChecks() {
  using namespace scratchbird::parser::sqlite;
  if (!Expect(DatatypeSurfaces().size() == 8,
              "SQLite datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 10,
              "SQLite builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 7,
              "SQLite catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 9,
              "SQLite diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(SqlitePackageIdentityJson(), "\"function_api_rows\":96") &&
                  Contains(SqlitePackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "SQLite package identity mismatch")) return false;

  const auto tokens = LexTokens("select \"name\", ?1, :v -- c\n");
  bool saw_quoted = false;
  bool saw_parameter = false;
  bool saw_comment = false;
  for (const auto& token : tokens) {
    saw_quoted = saw_quoted || token.kind == "quoted_identifier";
    saw_parameter = saw_parameter || token.kind == "parameter_or_variable";
    saw_comment = saw_comment || token.kind == "line_comment";
  }
  if (!Expect(saw_quoted && saw_parameter && saw_comment,
              "SQLite lexer did not classify quoted identifiers, parameters, and comments")) {
    return false;
  }

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "sqlite", "select json_extract(doc, '$.a') from t",
                   "sqlite.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "sqlite", "with q as (select 1) select * from q",
                   "sqlite.query.with", "WITH")) return false;
  if (!ExpectRoute(parse, "sqlite", "pragma table_info(t)",
                   "sqlite.pragma.generic", "PRAGMA", true)) return false;
  if (!ExpectRoute(parse, "sqlite", "create virtual table ft using fts5(body)",
                   "sqlite.virtual_table.create", "CREATE_VIRTUAL", true)) return false;
  if (!ExpectRoute(parse, "sqlite", "create temp table t(id integer)",
                   "sqlite.ddl.create_temp", "CREATE_TEMP")) return false;
  if (!ExpectRoute(parse, "sqlite", "create index ix_t_id on t(id)",
                   "sqlite.ddl.create_index", "CREATE_INDEX")) return false;
  if (!ExpectRoute(parse, "sqlite", "begin immediate",
                   "sqlite.transaction.begin", "BEGIN")) return false;
  if (!ExpectRoute(parse, "sqlite", "insert into t values (1)",
                   "sqlite.dml.insert", "INSERT")) return false;
  if (!ExpectUnsupportedDenied(parse, "sqlite", "vacuum into '/tmp/x.db'",
                               "sqlite.bulk_io.vacuum_into", "VACUUM",
                               "SQLITE.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "sqlite", "vacuum",
                               "sqlite.maintenance.vacuum", "VACUUM",
                               "SQLITE.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "sqlite", "analyze",
                               "sqlite.maintenance.analyze", "ANALYZE",
                               "SQLITE.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "sqlite", "reindex",
                               "sqlite.maintenance.reindex", "REINDEX",
                               "SQLITE.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectRoute(parse, "sqlite", "attach database '/tmp/x.db' as aux",
                   "sqlite.lifecycle.attach_database", "ATTACH",
                   false, false, false, true)) return false;
  const auto load_extension = ParseStatement("select load_extension('/tmp/ext.so')");
  if (!Expect(load_extension.ok && load_extension.fail_closed_refusal,
              "SQLite load_extension did not fail closed")) return false;
  if (!ExpectDiagnostic(load_extension.message_vector_json,
                        "SQLITE.AUTHORITY.EXTENSION_DENIED",
                        "SQLite load_extension")) return false;
  if (!ExpectRoute(parse, "sqlite", "select load_extension_name from t",
                   "sqlite.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "sqlite", "select 'load_extension(''/tmp/x'')'",
                   "sqlite.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "sqlite", "create table load_extension_name(id integer)",
                   "sqlite.ddl.create", "CREATE_TABLE")) return false;
  if (!CompatibilityWorkerSessionChecks("SQLite", Profile(), "sqlite")) return false;
  if (!ExpectOwnUnsupported(parse, "sqlite", "unsupported_sqlite_command",
                            "SQLITE.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool MariadbChecks() {
  using namespace scratchbird::parser::mariadb;
  if (!Expect(DatatypeSurfaces().size() == 12,
              "MariaDB datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 12,
              "MariaDB builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 9,
              "MariaDB catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 14,
              "MariaDB diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(MariadbPackageIdentityJson(), "\"function_api_rows\":141") &&
                  Contains(MariadbPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "MariaDB package identity mismatch")) return false;

  const auto tokens = LexTokens("select `Mixed Name`, @v, @@version # comment\n");
  bool saw_quoted = false;
  bool saw_variable = false;
  bool saw_comment = false;
  for (const auto& token : tokens) {
    saw_quoted = saw_quoted || token.kind == "quoted_identifier";
    saw_variable = saw_variable || token.kind == "parameter_or_variable";
    saw_comment = saw_comment || token.kind == "line_comment";
  }
  if (!Expect(saw_quoted && saw_variable && saw_comment,
              "MariaDB lexer did not classify quoted identifiers, variables, and comments")) {
    return false;
  }

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "mariadb", "select json_value(doc, '$.a') from t",
                   "mariadb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "mariadb", "create sequence seq start with 1",
                   "mariadb.sequence.create", "CREATE_SEQUENCE")) return false;
  if (!ExpectRoute(parse, "mariadb", "select next value for seq",
                   "mariadb.sequence.next_value_for", "SELECT")) return false;
  if (!ExpectRoute(parse, "mariadb", "insert into t values (1) returning id",
                   "mariadb.dml.returning", "INSERT")) return false;
  if (!ExpectRoute(parse, "mariadb", "handler t open",
                   "mariadb.handler.cursor", "HANDLER", true)) return false;
  if (!ExpectRoute(parse, "mariadb", "show tables",
                   "mariadb.catalog_overlay.show", "SHOW", false, true)) return false;
  if (!ExpectRoute(parse, "mariadb", "change replication source to source_host='h'",
                   "mariadb.replication.change_source", "CHANGE", true)) return false;
  if (!ExpectRoute(parse, "mariadb", "start replica",
                   "mariadb.replication.start_replica", "START", true)) return false;
  if (!ExpectRoute(parse, "mariadb", "show replica status",
                   "mariadb.replication.show_replica_status", "SHOW", true)) return false;
  if (!ExpectRoute(parse, "mariadb", "reset master",
                   "mariadb.replication.reset_master", "RESET", true)) return false;
  if (!ExpectRoute(parse, "mariadb",
                   "load data local infile 'client.csv' into table t",
                   "mariadb.bulk_io.load_data_local_infile", "LOAD_DATA", true)) return false;
  if (!ExpectRoute(parse, "mariadb",
                   "LOAD DATA LOCAL INFILE 'client.tsv' INTO TABLE t FIELDS TERMINATED BY '\t'",
                   "mariadb.bulk_io.load_data_local_infile", "LOAD_DATA", true)) return false;
  if (!ExpectRoute(parse, "mariadb",
                   "load data low_priority local infile 'client.csv' into table t",
                   "mariadb.bulk_io.load_data_local_infile", "LOAD_DATA", true)) return false;
  if (!ExpectRoute(parse, "mariadb",
                   "load data concurrent local infile 'client.csv' into table t",
                   "mariadb.bulk_io.load_data_local_infile", "LOAD_DATA", true)) return false;
  if (!ExpectUnsupportedDenied(parse, "mariadb", "analyze table t1",
                               "mariadb.maintenance.analyze_table", "ANALYZE",
                               "MARIADB.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "mariadb", "optimize table t1",
                               "mariadb.maintenance.optimize_table", "OPTIMIZE",
                               "MARIADB.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "mariadb", "check table t1",
                               "mariadb.maintenance.check_table", "CHECK",
                               "MARIADB.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "mariadb", "repair table t1",
                               "mariadb.maintenance.repair_table", "REPAIR",
                               "MARIADB.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "mariadb", "flush tables",
                               "mariadb.maintenance.flush", "FLUSH",
                               "MARIADB.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectRoute(parse, "mariadb", "install soname 'ha_example'",
                   "mariadb.plugin.install_soname", "INSTALL",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mariadb", "kill query 42",
                   "mariadb.session_admin.kill", "KILL",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mariadb", "select * from t into outfile '/tmp/x'",
                   "mariadb.bulk_io.select_into_outfile", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mariadb", "select * from t into dumpfile '/tmp/x'",
                   "mariadb.bulk_io.select_into_dumpfile", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mariadb", "select load_file('/tmp/x')",
                   "mariadb.bulk_io.load_file", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mariadb", "do load_file('/tmp/x')",
                   "mariadb.bulk_io.load_file", "DO",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mariadb",
                   "insert into t values (load_file('/tmp/x'))",
                   "mariadb.bulk_io.load_file", "INSERT",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mariadb",
                   "load data low_priority infile '/tmp/x.csv' into table t",
                   "mariadb.bulk_io.load_data_infile", "LOAD_DATA",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "mariadb",
                   "load data concurrent infile '/tmp/x.csv' into table t",
                   "mariadb.bulk_io.load_data_infile", "LOAD_DATA",
                   false, false, false, true)) return false;
  if (!ExpectOwnUnsupported(parse, "mariadb",
                            "load data locality infile 'client.csv' into table t",
                            "MARIADB.PARSE.UNSUPPORTED_SURFACE")) return false;
  if (!ExpectRoute(parse, "mariadb", "select 'load data local infile client.csv'",
                   "mariadb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "mariadb", "select 'select load_file(''/tmp/x'')'",
                   "mariadb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "mariadb", "select 'select * from t into outfile ''/tmp/x'''",
                   "mariadb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "mariadb", "select load_file_name from t",
                   "mariadb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "mariadb", "select into_outfile from t",
                   "mariadb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "mariadb", "select into_dumpfile from t",
                   "mariadb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "mariadb", "create table load_file_name (id int)",
                   "mariadb.ddl.create", "CREATE_TABLE")) return false;
  const auto load_data = ParseStatement("load data infile '/tmp/x.csv' into table t");
  if (!Expect(load_data.ok && load_data.fail_closed_refusal,
              "MariaDB LOAD DATA did not fail closed")) return false;
  if (!ExpectDiagnostic(load_data.message_vector_json,
                        "MARIADB.AUTHORITY.FILE_IO_DENIED",
                        "MariaDB LOAD DATA")) return false;
  const auto load_file = ParseStatement("select load_file('/tmp/x')");
  if (!Expect(load_file.ok && load_file.fail_closed_refusal,
              "MariaDB LOAD_FILE did not fail closed")) return false;
  if (!ExpectDiagnostic(load_file.message_vector_json,
                        "MARIADB.AUTHORITY.FILE_IO_DENIED",
                        "MariaDB LOAD_FILE")) return false;
  const auto catalog_write =
      ParseStatement("update information_schema.tables set table_name='x'");
  if (!Expect(!catalog_write.ok,
              "MariaDB information_schema mutation was accepted")) return false;
  if (!ExpectDiagnostic(catalog_write.message_vector_json,
                        "MARIADB.CATALOG_OVERLAY.READ_ONLY",
                        "MariaDB catalog mutation")) return false;
  if (!CompatibilityWorkerSessionChecks("MariaDB", Profile(), "mariadb")) return false;
  if (!ExpectOwnUnsupported(parse, "mariadb", "unsupported_mariadb_command",
                            "MARIADB.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool DuckdbChecks() {
  using namespace scratchbird::parser::duckdb;
  if (!Expect(DatatypeSurfaces().size() == 11,
              "DuckDB datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 12,
              "DuckDB builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 8,
              "DuckDB catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 12,
              "DuckDB diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(DuckdbPackageIdentityJson(), "\"function_api_rows\":132") &&
                  Contains(DuckdbPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "DuckDB package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "duckdb", "select date_part('year', current_date)",
                   "duckdb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "duckdb", "pivot t on a using sum(b)",
                   "duckdb.query.pivot", "PIVOT")) return false;
  if (!ExpectRoute(parse, "duckdb", "summarize t",
                   "duckdb.optimizer.summarize", "SUMMARIZE", false, true)) return false;
  if (!ExpectRoute(parse, "duckdb", "copy t from stdin",
                   "duckdb.bulk_io.copy", "COPY", true)) return false;
  if (!ExpectRoute(parse, "duckdb", "create secret scratchbird_secret",
                   "duckdb.security.create_secret", "CREATE_SECRET", true)) return false;
  if (!ExpectRoute(parse, "duckdb", "install httpfs",
                   "duckdb.extension.install", "INSTALL",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "duckdb", "select * from read_csv('/tmp/x.csv')",
                   "duckdb.external_scan.read_csv", "SELECT",
                   true)) return false;
  if (!ExpectRoute(parse, "duckdb", "select * from read_parquet('s3://bucket/x.parquet')",
                   "duckdb.external_scan.read_parquet", "SELECT",
                   true)) return false;
  if (!ExpectRoute(parse, "duckdb", "select * from read_json('/tmp/x.json')",
                   "duckdb.external_scan.read_json", "SELECT",
                   true)) return false;
  if (!ExpectRoute(parse, "duckdb",
                   "select * from 's3://bucket/x.parquet'",
                   "duckdb.external_scan.s3", "SELECT",
                   true)) return false;
  if (!ExpectRoute(parse, "duckdb",
                   "select * from 'http://example.test/x.csv'",
                   "duckdb.external_scan.http", "SELECT",
                   true)) return false;
  if (!ExpectRoute(parse, "duckdb",
                   "select * from 'https://example.test/x.csv'",
                   "duckdb.external_scan.http", "SELECT",
                   true)) return false;
  if (!ExpectRoute(parse, "duckdb",
                   "create table imported as select * from 's3://bucket/x.parquet'",
                   "duckdb.external_scan.s3", "CREATE_TABLE",
                   true)) return false;
  if (!ExpectRoute(parse, "duckdb",
                   "create table imported as from 'https://example.test/x.csv'",
                   "duckdb.external_scan.http", "CREATE_TABLE",
                   true)) return false;
  if (!ExpectRoute(parse, "duckdb",
                   "insert into imported select * from 's3://bucket/x.parquet'",
                   "duckdb.external_scan.s3", "INSERT",
                   true)) return false;
  if (!ExpectRoute(parse, "duckdb",
                   "describe from 'https://example.test/x.csv'",
                   "duckdb.external_scan.http", "DESCRIBE",
                   true)) return false;
  if (!ExpectRoute(parse, "duckdb", "select read_csv_name from t",
                   "duckdb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "duckdb", "select 'read_parquet(''/tmp/x'')'",
                   "duckdb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "duckdb", "select s3_bucket from t",
                   "duckdb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "duckdb", "select http_url from t",
                   "duckdb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "duckdb",
                   "select 's3://bucket/x.parquet'",
                   "duckdb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "duckdb",
                   "select 'http://example.test/x.csv'",
                   "duckdb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "duckdb",
                   "select * from t -- s3://bucket/x.parquet",
                   "duckdb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "duckdb",
                   "select * from t /* https://example.test/x.csv */",
                   "duckdb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "duckdb", "create table read_json_name(id integer)",
                   "duckdb.ddl.create", "CREATE_TABLE")) return false;
  if (!ExpectRoute(parse, "duckdb", "set memory_limit to '1GB'",
                   "duckdb.session.set", "SET")) return false;
  if (!ExpectRoute(parse, "duckdb", "copy t to '/tmp/x.csv'",
                   "duckdb.bulk_io.file_to", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("copy t to '/tmp/x.csv'").message_vector_json,
                        "DUCKDB.AUTHORITY.FILE_IO_DENIED",
                        "DuckDB COPY TO file")) return false;
  if (!ExpectRoute(parse, "duckdb", "copy t from '/tmp/x.csv'",
                   "duckdb.bulk_io.file_from", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("copy t from '/tmp/x.csv'").message_vector_json,
                        "DUCKDB.AUTHORITY.FILE_IO_DENIED",
                        "DuckDB COPY FROM file")) return false;
  if (!ExpectRoute(parse, "duckdb", "select * from '/tmp/x.csv'",
                   "duckdb.bulk_io.file_from", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("select * from '/tmp/x.csv'").message_vector_json,
                        "DUCKDB.AUTHORITY.FILE_IO_DENIED",
                        "DuckDB direct file scan")) return false;
  if (!ExpectRoute(parse, "duckdb", "copy t from 's3://bucket/x.parquet'",
                   "duckdb.bulk_io.file_from", "COPY",
                   false, false, false, true)) return false;
  const auto checkpoint = ParseStatement("checkpoint");
  if (!Expect(checkpoint.ok && checkpoint.fail_closed_refusal,
              "DuckDB CHECKPOINT did not fail closed")) return false;
  if (!ExpectDiagnostic(checkpoint.message_vector_json,
                        "DUCKDB.AUTHORITY.UNSUPPORTED_DENIED",
                        "DuckDB CHECKPOINT")) return false;
  if (!CompatibilityWorkerSessionChecks("DuckDB", Profile(), "duckdb")) return false;
  if (!ExpectOwnUnsupported(parse, "duckdb", "unsupported_duckdb_command",
                            "DUCKDB.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool ClickhouseChecks() {
  using namespace scratchbird::parser::clickhouse;
  if (!Expect(DatatypeSurfaces().size() == 12,
              "ClickHouse datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 13,
              "ClickHouse builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 10,
              "ClickHouse catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 13,
              "ClickHouse diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(ClickhousePackageIdentityJson(), "\"function_api_rows\":178") &&
                  Contains(ClickhousePackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "ClickHouse package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "clickhouse", "select count() from t",
                   "clickhouse.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "clickhouse", "show tables",
                   "clickhouse.catalog_overlay.show", "SHOW", false, true)) return false;
  if (!ExpectRoute(parse, "clickhouse", "use analytics",
                   "clickhouse.session.use_database", "USE")) return false;
  if (!ExpectRoute(parse, "clickhouse", "create dictionary d",
                   "clickhouse.connector.dictionary.create", "CREATE_DICTIONARY",
                   false, false, false, true)) return false;
  if (!ExpectUnsupportedDenied(parse, "clickhouse", "optimize table t final",
                               "clickhouse.maintenance.optimize", "OPTIMIZE",
                               "CLICKHOUSE.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "clickhouse", "check table t",
                               "clickhouse.maintenance.check_table", "CHECK",
                               "CLICKHOUSE.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "clickhouse", "system reload dictionaries",
                               "clickhouse.system.command", "SYSTEM",
                               "CLICKHOUSE.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "clickhouse", "kill query where query_id = 'q'",
                               "clickhouse.system.kill", "KILL",
                               "CLICKHOUSE.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectRoute(parse, "clickhouse", "select * from file('/tmp/x.csv')",
                   "clickhouse.external_io.file_function", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("select * from file('/tmp/x.csv')").message_vector_json,
                        "CLICKHOUSE.AUTHORITY.EXTERNAL_IO_DENIED",
                        "ClickHouse file table function")) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "select * from url('https://example.invalid/data.csv')",
                   "clickhouse.external_io.url_function", "SELECT",
                   true)) return false;
  if (!ExpectRoute(parse, "clickhouse", "select * from s3('s3://bucket/file')",
                   "clickhouse.external_io.s3_function", "SELECT",
                   true)) return false;
  if (!ExpectRoute(parse, "clickhouse", "select * from hdfs('hdfs://nn/path')",
                   "clickhouse.external_io.hdfs_function", "SELECT",
                   true)) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "select * from remote('cluster', db, table)",
                   "clickhouse.distributed.remote_function", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("select * from remote('cluster', db, table)").message_vector_json,
                        "CLICKHOUSE.AUTHORITY.DISTRIBUTED_DENIED",
                        "ClickHouse remote table function")) return false;
  if (!ExpectRoute(parse, "clickhouse", "insert into function null('TSV') select * from t",
                   "clickhouse.external_io.insert_function", "INSERT",
                   true)) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "create table kafka_events (id Int32) ENGINE = Kafka('broker:9092', 'topic', 'group', 'JSONEachRow')",
                   "clickhouse.etl.kafka", "CREATE_TABLE",
                   true)) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "CREATE TABLE t (id Int32) ENGINE=Kafka",
                   "clickhouse.etl.kafka", "CREATE_TABLE",
                   true)) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "CREATE TABLE t (id Int32) ENGINE Kafka",
                   "clickhouse.etl.kafka", "CREATE_TABLE",
                   true)) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "CREATE TABLE IF NOT EXISTS t (id Int32) ENGINE Kafka",
                   "clickhouse.etl.kafka", "CREATE_TABLE",
                   true)) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "CREATE OR REPLACE TABLE t (id Int32) ENGINE = Kafka",
                   "clickhouse.etl.kafka", "CREATE",
                   true)) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "CREATE TEMPORARY TABLE t (id Int32) ENGINE Kafka",
                   "clickhouse.etl.kafka", "CREATE",
                   true)) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "create table t (id Int32)\nENGINE   =   kafka('broker:9092', 'topic', 'group', 'JSONEachRow')",
                   "clickhouse.etl.kafka", "CREATE_TABLE",
                   true)) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "CREATE DATABASE db ENGINE = Kafka",
                   "clickhouse.ddl.create", "CREATE_DATABASE")) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "ATTACH TABLE t (id Int32) ENGINE = Kafka",
                   "clickhouse.storage.attach", "ATTACH",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "create table kafka_events (id Int32) engine=Memory",
                   "clickhouse.ddl.create", "CREATE_TABLE")) return false;
  if (!ExpectRoute(parse, "clickhouse", "create table t (kafka String) engine=Memory",
                   "clickhouse.ddl.create", "CREATE_TABLE")) return false;
  if (!ExpectRoute(parse, "clickhouse", "CREATE TABLE t (id Int32) ENGINE = KafkaLog",
                   "clickhouse.ddl.create", "CREATE_TABLE")) return false;
  if (!ExpectRoute(parse, "clickhouse", "select kafka_name from t",
                   "clickhouse.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "clickhouse", "select engine = kafka from t",
                   "clickhouse.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "clickhouse", "select 'ENGINE = Kafka(''broker'')'",
                   "clickhouse.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "clickhouse", "select 1 -- ENGINE = Kafka",
                   "clickhouse.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "clickhouse", "select 1 /* ENGINE = Kafka */",
                   "clickhouse.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "create table t on cluster c (id Int32) engine=Memory",
                   "clickhouse.distributed.cluster_clause", "CREATE_TABLE")) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "alter table t on cluster c add column x Int32",
                   "clickhouse.distributed.cluster_clause", "ALTER_TABLE")) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "drop table t on cluster c",
                   "clickhouse.distributed.cluster_clause", "DROP_TABLE")) return false;
  const auto cluster_clause =
      ParseStatement("create table t on cluster c (id Int32) engine=Memory");
  if (!Expect(cluster_clause.sblr_operation ==
                  "required_new:sblr.cluster.query.v1:cluster.query.plan_distributed" &&
              cluster_clause.engine_api_function == "cluster.query.plan_distributed" &&
              !cluster_clause.fail_closed_refusal &&
              !cluster_clause.parser_support_udr_route,
              "ClickHouse ON CLUSTER route lost provider-bound SBLR boundary")) {
    return false;
  }
  if (!ExpectRoute(parse, "clickhouse", "select file_name from t",
                   "clickhouse.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "clickhouse", "select url_value from t",
                   "clickhouse.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "clickhouse", "select s3_bucket_name from t",
                   "clickhouse.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "clickhouse", "select hdfs_path from t",
                   "clickhouse.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "clickhouse", "select remote_status from t",
                   "clickhouse.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "clickhouse", "select 'remote(''cluster'', db, table)'",
                   "clickhouse.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "clickhouse",
                   "create table cluster_events (id Int32) engine=Memory",
                   "clickhouse.ddl.create", "CREATE_TABLE")) return false;
  if (!ExpectRoute(parse, "clickhouse", "select cluster_name from system.clusters",
                   "clickhouse.query.select", "SELECT")) return false;
  if (!CompatibilityWorkerSessionChecks("ClickHouse", Profile(), "clickhouse")) return false;
  if (!ExpectOwnUnsupported(parse, "clickhouse", "unsupported_clickhouse_command",
                            "CLICKHOUSE.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool TidbChecks() {
  using namespace scratchbird::parser::tidb;
  if (!Expect(DatatypeSurfaces().size() == 12,
              "TiDB datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 12,
              "TiDB builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 9,
              "TiDB catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 14,
              "TiDB diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(TidbPackageIdentityJson(), "\"function_api_rows\":123") &&
                  Contains(TidbPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "TiDB package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "tidb", "select tidb_version()",
                   "tidb.builtin.version", "SELECT", false, true)) return false;
  if (!ExpectRoute(parse, "tidb", "split table t between (1) and (100) regions 4",
                   "tidb.placement.split_table", "SPLIT")) return false;
  if (!ExpectRoute(parse, "tidb", "create placement policy p constraints='[+region=us-east-1]'",
                   "tidb.placement.policy.create", "CREATE_PLACEMENT")) return false;
  if (!ExpectRoute(parse, "tidb", "create resource group rg ru_per_sec = 100",
                   "tidb.resource_group.create", "CREATE_RESOURCE", true)) return false;
  if (!ExpectRoute(parse, "tidb", "load data infile '/tmp/x.csv' into table t",
                   "tidb.bulk_io.load_data_infile", "LOAD_DATA",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "tidb", "load data local infile 'client.csv' into table t",
                   "tidb.bulk_io.load_data_local_infile", "LOAD_DATA",
                   true)) return false;
  if (!ExpectRoute(parse, "tidb",
                   "LOAD DATA LOCAL INFILE 'client.tsv' INTO TABLE t FIELDS TERMINATED BY '\t'",
                   "tidb.bulk_io.load_data_local_infile", "LOAD_DATA",
                   true)) return false;
  if (!ExpectRoute(parse, "tidb",
                   "load data low_priority local infile 'client.csv' into table t",
                   "tidb.bulk_io.load_data_local_infile", "LOAD_DATA",
                   true)) return false;
  if (!ExpectRoute(parse, "tidb",
                   "load data concurrent local infile 'client.csv' into table t",
                   "tidb.bulk_io.load_data_local_infile", "LOAD_DATA",
                   true)) return false;
  if (!ExpectRoute(parse, "tidb",
                   "load data low_priority infile '/tmp/x.csv' into table t",
                   "tidb.bulk_io.load_data_infile", "LOAD_DATA",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "tidb",
                   "load data concurrent infile '/tmp/x.csv' into table t",
                   "tidb.bulk_io.load_data_infile", "LOAD_DATA",
                   false, false, false, true)) return false;
  if (!ExpectOwnUnsupported(parse, "tidb",
                            "load data locality infile 'client.csv' into table t",
                            "TIDB.PARSE.UNSUPPORTED_SURFACE")) return false;
  if (!ExpectRoute(parse, "tidb", "select load_file('/tmp/x')",
                   "tidb.bulk_io.load_file", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "tidb", "do load_file('/tmp/x')",
                   "tidb.bulk_io.load_file", "DO",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "tidb",
                   "insert into t values (load_file('/tmp/x'))",
                   "tidb.bulk_io.load_file", "INSERT",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "tidb", "select * from t into outfile '/tmp/x'",
                   "tidb.bulk_io.select_into_outfile", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "tidb", "select 'load data local infile client.csv'",
                   "tidb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "tidb", "select 'select load_file(''/tmp/x'')'",
                   "tidb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "tidb", "select 'select * from t into outfile ''/tmp/x'''",
                   "tidb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "tidb", "select load_file_name from t",
                   "tidb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "tidb", "select into_outfile from t",
                   "tidb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "tidb", "create table load_file_name (id int)",
                   "tidb.ddl.create", "CREATE_TABLE")) return false;
  if (!ExpectRoute(parse, "tidb", "ticdc changefeed create --sink-uri kafka://broker/topic",
                   "tidb.cdc.ticdc", "TICDC",
                   true)) return false;
  if (!ExpectRoute(parse, "tidb", "changefeed create for table t",
                   "tidb.cdc.changefeed", "CHANGEFEED",
                   true)) return false;
  if (!ExpectRoute(parse, "tidb", "admin checksum table t",
                   "tidb.admin.checksum_table", "ADMIN",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "tidb", "backup database d to 'local:///tmp/d'",
                   "tidb.backup.backup", "BACKUP",
                   false, false, false, true)) return false;
  const auto catalog_write =
      ParseStatement("update information_schema.tables set table_name='x'");
  if (!Expect(!catalog_write.ok,
              "TiDB information_schema mutation was accepted")) return false;
  if (!ExpectDiagnostic(catalog_write.message_vector_json,
                        "TIDB.CATALOG_OVERLAY.READ_ONLY",
                        "TiDB catalog mutation")) return false;
  if (!CompatibilityWorkerSessionChecks("TiDB", Profile(), "tidb")) return false;
  if (!ExpectOwnUnsupported(parse, "tidb", "unsupported_tidb_command",
                            "TIDB.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool VitessChecks() {
  using namespace scratchbird::parser::vitess;
  if (!Expect(DatatypeSurfaces().size() == 10,
              "Vitess datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 10,
              "Vitess builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 8,
              "Vitess catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 11,
              "Vitess diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(VitessPackageIdentityJson(), "\"function_api_rows\":123") &&
                  Contains(VitessPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "Vitess package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "vitess", "select keyspace_id from customer",
                   "vitess.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "vitess", "show vschema",
                   "vitess.catalog_overlay.show_vschema", "SHOW", false, true)) return false;
  if (!ExpectRoute(parse, "vitess", "move tables commerce.customer",
                   "vitess.vreplication.move_tables", "MOVE", true)) return false;
  if (!ExpectRoute(parse, "vitess", "reshard commerce.customer",
                   "vitess.vreplication.reshard", "RESHARD", true)) return false;
  if (!ExpectRoute(parse, "vitess", "vdiff commerce.customer",
                   "vitess.vreplication.vdiff", "VDIFF", true)) return false;
  if (!ExpectRoute(parse, "vitess", "load data local infile 'client.csv' into table t",
                   "vitess.bulk_io.load_data_local_infile", "LOAD_DATA", true)) return false;
  if (!ExpectRoute(parse, "vitess",
                   "LOAD DATA LOCAL INFILE 'client.tsv' INTO TABLE t FIELDS TERMINATED BY '\t'",
                   "vitess.bulk_io.load_data_local_infile", "LOAD_DATA",
                   true)) return false;
  if (!ExpectRoute(parse, "vitess",
                   "load data low_priority local infile 'client.csv' into table t",
                   "vitess.bulk_io.load_data_local_infile", "LOAD_DATA",
                   true)) return false;
  if (!ExpectRoute(parse, "vitess",
                   "load data concurrent local infile 'client.csv' into table t",
                   "vitess.bulk_io.load_data_local_infile", "LOAD_DATA",
                   true)) return false;
  if (!ExpectRoute(parse, "vitess", "load data infile '/tmp/x.csv' into table t",
                   "vitess.bulk_io.load_data_infile", "LOAD_DATA",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "vitess",
                   "load data low_priority infile '/tmp/x.csv' into table t",
                   "vitess.bulk_io.load_data_infile", "LOAD_DATA",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "vitess",
                   "load data concurrent infile '/tmp/x.csv' into table t",
                   "vitess.bulk_io.load_data_infile", "LOAD_DATA",
                   false, false, false, true)) return false;
  if (!ExpectOwnUnsupported(parse, "vitess",
                            "load data locality infile 'client.csv' into table t",
                            "VITESS.PARSE.UNSUPPORTED_SURFACE")) return false;
  if (!ExpectRoute(parse, "vitess", "select load_file('/tmp/x')",
                   "vitess.bulk_io.load_file", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "vitess", "do load_file('/tmp/x')",
                   "vitess.bulk_io.load_file", "DO",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "vitess",
                   "insert into t values (load_file('/tmp/x'))",
                   "vitess.bulk_io.load_file", "INSERT",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "vitess", "select * from t into outfile '/tmp/x'",
                   "vitess.bulk_io.select_into_outfile", "SELECT",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "vitess", "select 'load data local infile client.csv'",
                   "vitess.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "vitess", "select 'select load_file(''/tmp/x'')'",
                   "vitess.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "vitess", "select 'select * from t into outfile ''/tmp/x'''",
                   "vitess.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "vitess", "select load_file_name from t",
                   "vitess.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "vitess", "select into_outfile from t",
                   "vitess.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "vitess", "create table load_file_name (id int)",
                   "vitess.ddl.create", "CREATE_TABLE")) return false;
  if (!ExpectRoute(parse, "vitess", "alter vschema add vindex customer_hash",
                   "vitess.vschema.alter", "ALTER_VSCHEMA", true)) return false;
  if (!ExpectRoute(parse, "vitess", "vtctl reparent shard commerce/0",
                   "vitess.topology.reparent", "VTCTL")) return false;
  if (!CompatibilityWorkerSessionChecks("Vitess", Profile(), "vitess")) return false;
  if (!ExpectOwnUnsupported(parse, "vitess", "unsupported_vitess_command",
                            "VITESS.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool CockroachdbChecks() {
  using namespace scratchbird::parser::cockroachdb;
  if (!Expect(DatatypeSurfaces().size() == 12,
              "CockroachDB datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 10,
              "CockroachDB builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 10,
              "CockroachDB catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 12,
              "CockroachDB diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(CockroachdbPackageIdentityJson(), "\"function_api_rows\":4") &&
                  Contains(CockroachdbPackageIdentityJson(), "\"unsupported_rows\":2") &&
                  Contains(CockroachdbPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "CockroachDB package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "cockroachdb", "select crdb_internal.node_id()",
                   "cockroachdb.catalog_overlay.crdb_internal", "SELECT", false, true)) return false;
  if (!ExpectRoute(parse, "cockroachdb", "show ranges from table t",
                   "cockroachdb.catalog_overlay.show_ranges", "SHOW", false, true)) return false;
  if (!ExpectRoute(parse, "cockroachdb", "create changefeed for table t",
                   "cockroachdb.changefeed.create", "CREATE_CHANGEFEED", true)) return false;
  if (!ExpectRoute(parse, "cockroachdb", "create foreign data wrapper scratchbird_fdw",
                   "cockroachdb.connector.fdw.create", "CREATE_FOREIGN", true)) return false;
  if (!ExpectRoute(parse, "cockroachdb", "set cluster setting sql.defaults.vectorize = on",
                   "cockroachdb.cluster.setting.set", "SET")) return false;
  if (!ExpectRoute(parse, "cockroachdb", "backup database d to 'nodelocal://1/a'",
                   "cockroachdb.backup.backup", "BACKUP",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "cockroachdb", "copy public.t from stdin",
                   "cockroachdb.logical_stream.copy_from_stdin", "COPY",
                   true)) return false;
  if (!ExpectRoute(parse, "cockroachdb", "copy public.t to stdout",
                   "cockroachdb.logical_stream.copy_to_stdout", "COPY",
                   true)) return false;
  if (!ExpectRoute(parse, "cockroachdb", "copy public.t to '/tmp/x.csv'",
                   "cockroachdb.bulk_io.copy_to_file", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("copy public.t to '/tmp/x.csv'").message_vector_json,
                        "COCKROACHDB.AUTHORITY.FILE_IO_DENIED",
                        "CockroachDB COPY TO file")) return false;
  if (!ExpectRoute(parse, "cockroachdb", "copy public.t from '/tmp/x.csv'",
                   "cockroachdb.bulk_io.copy_from_file", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("copy public.t from '/tmp/x.csv'").message_vector_json,
                        "COCKROACHDB.AUTHORITY.FILE_IO_DENIED",
                        "CockroachDB COPY FROM file")) return false;
  if (!ExpectRoute(parse, "cockroachdb", "copy public.t to 'stdout'",
                   "cockroachdb.bulk_io.copy_to_file", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("copy public.t to 'stdout'").message_vector_json,
                        "COCKROACHDB.AUTHORITY.FILE_IO_DENIED",
                        "CockroachDB COPY TO quoted stdout")) return false;
  if (!ExpectRoute(parse, "cockroachdb", "copy public.t from 'stdin'",
                   "cockroachdb.bulk_io.copy_from_file", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("copy public.t from 'stdin'").message_vector_json,
                        "COCKROACHDB.AUTHORITY.FILE_IO_DENIED",
                        "CockroachDB COPY FROM quoted stdin")) return false;
  if (!ExpectRoute(parse, "cockroachdb",
                   "copy public.t from program 'cat /etc/passwd'",
                   "cockroachdb.bulk_io.copy_program", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement(
                            "copy public.t from program 'cat /etc/passwd'")
                            .message_vector_json,
                        "COCKROACHDB.AUTHORITY.PROGRAM_DENIED",
                        "CockroachDB COPY PROGRAM")) return false;
  if (!ExpectRoute(parse, "cockroachdb", "alter table public.t rename to stdout",
                   "cockroachdb.ddl.alter_table", "ALTER_TABLE")) return false;
  if (!ExpectRoute(parse, "cockroachdb", "select * from stdin",
                   "cockroachdb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "cockroachdb", "select experimental_relocate(1)",
                   "cockroachdb.cluster.experimental_relocate", "SELECT")) return false;
  if (!CompatibilityWorkerSessionChecks("CockroachDB", Profile(), "cockroachdb")) return false;
  if (!ExpectOwnUnsupported(parse, "cockroachdb", "unsupported_cockroachdb_command",
                            "COCKROACHDB.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool YugabytedbChecks() {
  using namespace scratchbird::parser::yugabytedb;
  if (!Expect(DatatypeSurfaces().size() == 12,
              "YugabyteDB datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 12,
              "YugabyteDB builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 10,
              "YugabyteDB catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 12,
              "YugabyteDB diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(YugabytedbPackageIdentityJson(), "\"function_api_rows\":477") &&
                  Contains(YugabytedbPackageIdentityJson(), "\"connector_operation_rows\":10") &&
                  Contains(YugabytedbPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "YugabyteDB package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "yugabytedb", "select yb_server_region()",
                   "yugabytedb.catalog_overlay.server_region", "SELECT", false, true)) return false;
  if (!ExpectRoute(parse, "yugabytedb", "create keyspace ks",
                   "yugabytedb.ycql.keyspace.create", "CREATE_KEYSPACE", true)) return false;
  if (!ExpectRoute(parse, "yugabytedb", "create table t (id int primary key) split into 3 tablets",
                   "yugabytedb.tablet.split_into", "CREATE_TABLE", true)) return false;
  if (!ExpectRoute(parse, "yugabytedb", "create foreign data wrapper scratchbird_fdw",
                   "yugabytedb.connector.fdw.create", "CREATE_FOREIGN",
                   true)) return false;
  if (!ExpectRoute(parse, "yugabytedb", "create cdc stream for table t",
                   "yugabytedb.cdc.create_stream", "CREATE_CDC", true)) return false;
  if (!ExpectRoute(parse, "yugabytedb", "backup database d to '/tmp/d'",
                   "yugabytedb.backup.backup", "BACKUP",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "yugabytedb", "copy public.t from stdin",
                   "yugabytedb.logical_stream.copy_from_stdin", "COPY",
                   true)) return false;
  if (!ExpectRoute(parse, "yugabytedb", "copy public.t to stdout",
                   "yugabytedb.logical_stream.copy_to_stdout", "COPY",
                   true)) return false;
  if (!ExpectRoute(parse, "yugabytedb", "copy public.t to '/tmp/x.csv'",
                   "yugabytedb.bulk_io.copy_to_file", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("copy public.t to '/tmp/x.csv'").message_vector_json,
                        "YUGABYTEDB.AUTHORITY.FILE_IO_DENIED",
                        "YugabyteDB COPY TO file")) return false;
  if (!ExpectRoute(parse, "yugabytedb", "copy public.t from '/tmp/x.csv'",
                   "yugabytedb.bulk_io.copy_from_file", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("copy public.t from '/tmp/x.csv'").message_vector_json,
                        "YUGABYTEDB.AUTHORITY.FILE_IO_DENIED",
                        "YugabyteDB COPY FROM file")) return false;
  if (!ExpectRoute(parse, "yugabytedb", "copy public.t to 'stdout'",
                   "yugabytedb.bulk_io.copy_to_file", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("copy public.t to 'stdout'").message_vector_json,
                        "YUGABYTEDB.AUTHORITY.FILE_IO_DENIED",
                        "YugabyteDB COPY TO quoted stdout")) return false;
  if (!ExpectRoute(parse, "yugabytedb", "copy public.t from 'stdin'",
                   "yugabytedb.bulk_io.copy_from_file", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement("copy public.t from 'stdin'").message_vector_json,
                        "YUGABYTEDB.AUTHORITY.FILE_IO_DENIED",
                        "YugabyteDB COPY FROM quoted stdin")) return false;
  if (!ExpectRoute(parse, "yugabytedb",
                   "copy public.t from program 'cat /etc/passwd'",
                   "yugabytedb.bulk_io.copy_program", "COPY",
                   false, false, false, true)) return false;
  if (!ExpectDiagnostic(ParseStatement(
                            "copy public.t from program 'cat /etc/passwd'")
                            .message_vector_json,
                        "YUGABYTEDB.AUTHORITY.PROGRAM_DENIED",
                        "YugabyteDB COPY PROGRAM")) return false;
  if (!ExpectRoute(parse, "yugabytedb", "alter table public.t rename to stdout",
                   "yugabytedb.ddl.alter", "ALTER_TABLE")) return false;
  if (!ExpectRoute(parse, "yugabytedb", "select * from stdin",
                   "yugabytedb.query.select", "SELECT")) return false;
  if (!CompatibilityWorkerSessionChecks("YugabyteDB", Profile(), "yugabytedb")) return false;
  if (!ExpectOwnUnsupported(parse, "yugabytedb", "unsupported_yugabytedb_command",
                            "YUGABYTEDB.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool CassandraChecks() {
  using namespace scratchbird::parser::cassandra;
  if (!Expect(DatatypeSurfaces().size() == 10,
              "Cassandra datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 10,
              "Cassandra builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 9,
              "Cassandra catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 10,
              "Cassandra diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(CassandraPackageIdentityJson(), "\"function_api_rows\":120") &&
                  Contains(CassandraPackageIdentityJson(), "\"policy_blocked_rows\":2") &&
                  Contains(CassandraPackageIdentityJson(), "\"trusted_udr_registration_rows\":18") &&
                  Contains(CassandraPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "Cassandra package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "cassandra", "select json * from ks.t",
                   "cassandra.query.select_json", "SELECT")) return false;
  if (!ExpectRoute(parse, "cassandra", "insert into ks.t (id) values (1)",
                   "cassandra.dml.insert", "INSERT")) return false;
  if (!ExpectRoute(parse, "cassandra",
                   "create keyspace ks with replication = {'class':'SimpleStrategy'}",
                   "cassandra.keyspace.create", "CREATE_KEYSPACE", true)) return false;
  if (!ExpectRoute(parse, "cassandra", "copy ks.t to '/tmp/t.csv'",
                   "cassandra.cqlsh.copy", "COPY", true)) return false;
  if (!ExpectRoute(parse, "cassandra", "describe keyspaces",
                   "cassandra.catalog.describe", "DESCRIBE", false, true)) return false;
  if (!ExpectUnsupportedDenied(parse, "cassandra", "repair ks",
                               "cassandra.admin.repair", "REPAIR",
                               "CASSANDRA.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!ExpectUnsupportedDenied(parse, "cassandra", "nodetool repair ks",
                               "cassandra.admin.nodetool", "NODETOOL",
                               "CASSANDRA.AUTHORITY.UNSUPPORTED_DENIED")) return false;
  if (!CompatibilityWorkerSessionChecks("Cassandra", Profile(), "cassandra")) return false;
  if (!ExpectOwnUnsupported(parse, "cassandra", "unsupported_cassandra_command",
                            "CASSANDRA.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool MongodbChecks() {
  using namespace scratchbird::parser::mongodb;
  if (!Expect(DatatypeSurfaces().size() == 10,
              "MongoDB datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 10,
              "MongoDB builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 9,
              "MongoDB catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 10,
              "MongoDB diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(MongodbPackageIdentityJson(), "\"function_api_rows\":120") &&
                  Contains(MongodbPackageIdentityJson(), "\"policy_blocked_rows\":3") &&
                  Contains(MongodbPackageIdentityJson(), "\"trusted_udr_registration_rows\":9") &&
                  Contains(MongodbPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "MongoDB package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "mongodb", "find users { status: 'A' }",
                   "mongodb.query.find", "FIND")) return false;
  if (!ExpectRoute(parse, "mongodb", "insert users { _id: 1, status: 'A' }",
                   "mongodb.dml.insert", "INSERT")) return false;
  if (!ExpectRoute(parse, "mongodb", "aggregate orders [ {$match:{status:'A'}}, {$out:'archived'} ]",
                   "mongodb.aggregate.out", "AGGREGATE", true)) return false;
  if (!ExpectRoute(parse, "mongodb", "eval function() { return 1; }",
                   "mongodb.script.eval", "EVAL", true)) return false;
  if (!ExpectRoute(parse, "mongodb", "watch orders",
                   "mongodb.cdc.watch", "WATCH", true)) return false;
  if (!ExpectRoute(parse, "mongodb",
                   "aggregate orders [ {$changeStream:{fullDocument:'updateLookup'}} ]",
                   "mongodb.cdc.change_stream", "AGGREGATE", true)) return false;
  if (!ExpectRoute(parse, "mongodb", "serverStatus",
                   "mongodb.catalog.server_status", "SERVERSTATUS", false, true)) return false;
  if (!ExpectRoute(parse, "mongodb", "sh.shardCollection('db.c',{_id:1})",
                   "mongodb.sharding.shard_collection", "SH")) return false;
  if (!CompatibilityWorkerSessionChecks("MongoDB", Profile(), "mongodb")) return false;
  if (!ExpectOwnUnsupported(parse, "mongodb", "unsupported_mongodb_command",
                            "MONGODB.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool RedisChecks() {
  using namespace scratchbird::parser::redis;
  if (!Expect(DatatypeSurfaces().size() == 10,
              "Redis datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 10,
              "Redis builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 9,
              "Redis catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 10,
              "Redis diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(RedisPackageIdentityJson(), "\"connector_operation_rows\":1") &&
                  Contains(RedisPackageIdentityJson(), "\"policy_blocked_rows\":3") &&
                  Contains(RedisPackageIdentityJson(), "\"trusted_udr_registration_rows\":13") &&
                  Contains(RedisPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "Redis package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "redis", "set account:1 active",
                   "redis.kv.set", "SET")) return false;
  if (!ExpectRoute(parse, "redis", "get account:1",
                   "redis.kv.get", "GET")) return false;
  if (!ExpectRoute(parse, "redis", "eval \"return redis.call('get', KEYS[1])\" 1 account:1",
                   "redis.script.eval", "EVAL", true)) return false;
  if (!ExpectRoute(parse, "redis", "save",
                   "redis.persistence.save", "SAVE", true)) return false;
  if (!ExpectRoute(parse, "redis", "module list",
                   "redis.module.command", "MODULE", true)) return false;
  if (!ExpectRoute(parse, "redis", "replicaof 127.0.0.1 6379",
                   "redis.replication.replicaof", "REPLICAOF", true)) return false;
  if (!ExpectRoute(parse, "redis", "xadd mystream * sensor-id 1234 temperature 19.8",
                   "redis.stream.xadd", "XADD", true)) return false;
  if (!ExpectRoute(parse, "redis", "xread streams mystream 0",
                   "redis.stream.xread", "XREAD", true)) return false;
  if (!ExpectRoute(parse, "redis", "cluster nodes",
                   "redis.cluster.command", "CLUSTER")) return false;
  if (!CompatibilityWorkerSessionChecks("Redis", Profile(), "redis")) return false;
  if (!ExpectOwnUnsupported(parse, "redis", "unsupported_redis_command",
                            "REDIS.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool OpensearchSqlPplChecks() {
  using namespace scratchbird::parser::opensearch_sql_ppl;
  if (!Expect(DatatypeSurfaces().size() == 10,
              "OpenSearch SQL/PPL datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 11,
              "OpenSearch SQL/PPL builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 9,
              "OpenSearch SQL/PPL catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 10,
              "OpenSearch SQL/PPL diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(OpensearchSqlPplPackageIdentityJson(), "\"parser_surface_rows\":27") &&
                  Contains(OpensearchSqlPplPackageIdentityJson(), "\"policy_blocked_rows\":2") &&
                  Contains(OpensearchSqlPplPackageIdentityJson(), "\"trusted_udr_registration_rows\":11") &&
                  Contains(OpensearchSqlPplPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "OpenSearch SQL/PPL package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "select count(*) from accounts",
                   "opensearch_sql_ppl.sql.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "source=accounts | stats count() by state",
                   "opensearch_sql_ppl.ppl.stats", "SOURCE")) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "source=accounts | lookup region_lookup state",
                   "opensearch_sql_ppl.ppl.lookup", "SOURCE", true)) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "source=accounts | join orders on account_id",
                   "opensearch_sql_ppl.ppl.join", "SOURCE", true)) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "ml predict model_id='m1'",
                   "opensearch_sql_ppl.ml.command", "ML", true)) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "source=accounts | ad threshold=3",
                   "opensearch_sql_ppl.ad.command", "SOURCE", true)) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "create index accounts",
                   "opensearch_sql_ppl.index.create", "CREATE_INDEX", true)) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "drop index accounts",
                   "opensearch_sql_ppl.index.drop", "DROP_INDEX", true)) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "show datasources",
                   "opensearch_sql_ppl.catalog.show_datasources", "SHOW", false, true)) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl",
                   "POST /_plugins/_sql {\"query\":\"select count(*) from accounts\"}",
                   "opensearch_sql_ppl.rest.post", "POST", true)) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl",
                   "GET /_plugins/_ppl/_grammar",
                   "opensearch_sql_ppl.rest.get", "GET", true)) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl",
                   "PUT /_plugins/_query/settings {\"transient\":{}}",
                   "opensearch_sql_ppl.rest.put", "PUT", true)) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl",
                   "DELETE /_plugins/_query/_datasources/my_prometheus",
                   "opensearch_sql_ppl.rest.delete", "DELETE", true)) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "GET /_cluster/health",
                   "opensearch_sql_ppl.cluster.get", "GET")) return false;
  if (!ExpectOwnUnsupported(parse, "opensearch_sql_ppl",
                            "POST /accounts/_search {\"query\":{\"match_all\":{}}}",
                            "OPENSEARCH_SQL_PPL.PARSE.UNSUPPORTED_SURFACE")) return false;
  if (!ExpectOwnUnsupported(parse, "opensearch_sql_ppl",
                            "POST /_bulk {\"index\":{}}",
                            "OPENSEARCH_SQL_PPL.PARSE.UNSUPPORTED_SURFACE")) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "select 'GET /accounts/_search'",
                   "opensearch_sql_ppl.sql.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "delete from accounts where note = 'GET /_search'",
                   "opensearch_sql_ppl.sql.delete", "DELETE")) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "source=idx | fields stats_value",
                   "opensearch_sql_ppl.ppl.source", "SOURCE")) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "source=idx | eval ad_score=1",
                   "opensearch_sql_ppl.ppl.source", "SOURCE")) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "source=idx | fields lookup_value, join_key, ml_score",
                   "opensearch_sql_ppl.ppl.source", "SOURCE")) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "source=idx | eval note='lookup join stats ML AD'",
                   "opensearch_sql_ppl.ppl.source", "SOURCE")) return false;
  if (!ExpectRoute(parse, "opensearch_sql_ppl", "source=idx | eval score=1 -- lookup join stats ML AD",
                   "opensearch_sql_ppl.ppl.source", "SOURCE")) return false;
  if (!CompatibilityWorkerSessionChecks("OpenSearch SQL/PPL", Profile(), "opensearch_sql_ppl")) return false;
  if (!ExpectOwnUnsupported(parse, "opensearch_sql_ppl", "unsupported_opensearch_command",
                            "OPENSEARCH_SQL_PPL.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool OpensearchChecks() {
  using namespace scratchbird::parser::opensearch;
  if (!Expect(DatatypeSurfaces().size() == 8,
              "OpenSearch datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 8,
              "OpenSearch builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 10,
              "OpenSearch catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 10,
              "OpenSearch diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(OpensearchPackageIdentityJson(), "\"function_api_rows\":96") &&
                  Contains(OpensearchPackageIdentityJson(), "\"policy_blocked_rows\":3") &&
                  Contains(OpensearchPackageIdentityJson(), "\"trusted_udr_registration_rows\":5") &&
                  Contains(OpensearchPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "OpenSearch package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "opensearch", "POST /accounts/_search {\"query\":{\"match_all\":{}}}",
                   "opensearch.search.query", "POST")) return false;
  if (!ExpectRoute(parse, "opensearch", "POST /_msearch {\"query\":{\"match_all\":{}}}",
                   "opensearch.search.multi", "POST", true)) return false;
  if (!ExpectRoute(parse, "opensearch", "POST /_bulk {\"index\":{}}",
                   "opensearch.bulk.write", "POST", true)) return false;
  if (!ExpectRoute(parse, "opensearch", "GET /accounts/_mget",
                   "opensearch.document.multi_get", "GET")) return false;
  if (!ExpectRoute(parse, "opensearch", "PUT /accounts {\"settings\":{}}",
                   "opensearch.index.create", "PUT")) return false;
  if (!ExpectRoute(parse, "opensearch", "GET /accounts/_mapping",
                   "opensearch.catalog.mapping", "GET", false, true)) return false;
  if (!ExpectRoute(parse, "opensearch", "POST /_aliases {\"actions\":[]}",
                   "opensearch.catalog.aliases", "POST", true)) return false;
  if (!ExpectRoute(parse, "opensearch", "PUT /_ingest/pipeline/default {\"processors\":[]}",
                   "opensearch.ingest.pipeline", "PUT", true)) return false;
  if (!ExpectRoute(parse, "opensearch", "GET /_plugins/_security/api/roles",
                   "opensearch.security.route", "GET", true)) return false;
  if (!ExpectRoute(parse, "opensearch", "GET /_cat/indices",
                   "opensearch.catalog.cat", "GET", false, true)) return false;
  if (!ExpectRoute(parse, "opensearch", "GET /_cluster/health",
                   "opensearch.admin.cluster_health", "GET")) return false;
  if (!ExpectRoute(parse, "opensearch", "POST /accounts {\"note\":\"_bulk _search _mget\"}",
                   "opensearch.document.write", "POST")) return false;
  if (!ExpectRoute(parse, "opensearch", "POST /accounts/_bulk_stats {\"doc\":{}}",
                   "opensearch.document.write", "POST")) return false;
  if (!ExpectRoute(parse, "opensearch", "POST /accounts -- _search _bulk _mget",
                   "opensearch.document.write", "POST")) return false;
  if (!CompatibilityWorkerSessionChecks("OpenSearch", Profile(), "opensearch")) return false;
  if (!ExpectOwnUnsupported(parse, "opensearch", "unsupported_opensearch_rest_command",
                            "OPENSEARCH.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool Neo4jChecks() {
  using namespace scratchbird::parser::neo4j;
  if (!Expect(DatatypeSurfaces().size() == 6,
              "Neo4j datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 8,
              "Neo4j builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 9,
              "Neo4j catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 10,
              "Neo4j diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(Neo4jPackageIdentityJson(), "\"policy_blocked_rows\":3") &&
                  Contains(Neo4jPackageIdentityJson(), "\"trusted_udr_registration_rows\":10") &&
                  Contains(Neo4jPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "Neo4j package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "neo4j", "match (n:Account) return n",
                   "neo4j.query.match", "MATCH")) return false;
  if (!ExpectRoute(parse, "neo4j", "create (n:Account {id: 1})",
                   "neo4j.graph.create", "CREATE")) return false;
  if (!ExpectRoute(parse, "neo4j",
                   "create constraint account_id if not exists for (a:Account) require a.id is unique",
                   "neo4j.schema.constraint.create", "CREATE_CONSTRAINT", true)) return false;
  if (!ExpectRoute(parse, "neo4j", "call db.labels() yield label return label",
                   "neo4j.procedure.call", "CALL", true)) return false;
  if (!ExpectRoute(parse, "neo4j",
                   "load csv from 'https://example.invalid/accounts.csv' as row return row",
                   "neo4j.client_file.load_csv", "LOAD", true)) return false;
  if (!ExpectRoute(parse, "neo4j", "show indexes",
                   "neo4j.catalog.show", "SHOW", false, true)) return false;
  if (!ExpectRoute(parse, "neo4j", "show servers",
                   "neo4j.admin.show_servers", "SHOW")) return false;
  if (!CompatibilityWorkerSessionChecks("Neo4j", Profile(), "neo4j")) return false;
  if (!ExpectOwnUnsupported(parse, "neo4j", "unsupported_neo4j_command",
                            "NEO4J.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool InfluxdbChecks() {
  using namespace scratchbird::parser::influxdb;
  if (!Expect(DatatypeSurfaces().size() == 7,
              "InfluxDB datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 7,
              "InfluxDB builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 9,
              "InfluxDB catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 10,
              "InfluxDB diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(InfluxdbPackageIdentityJson(), "\"function_api_rows\":84") &&
                  Contains(InfluxdbPackageIdentityJson(), "\"policy_blocked_rows\":2") &&
                  Contains(InfluxdbPackageIdentityJson(), "\"trusted_udr_registration_rows\":10") &&
                  Contains(InfluxdbPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "InfluxDB package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "influxdb", "select mean(value) from cpu",
                   "influxdb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "influxdb", "cpu,host=a value=1",
                   "influxdb.write.line_protocol", "CPU", true)) return false;
  if (!ExpectRoute(parse, "influxdb", "create retention policy rp on metrics duration 7d replication 1",
                   "influxdb.retention_policy.create", "CREATE_RETENTION", true)) return false;
  if (!ExpectRoute(parse, "influxdb", "show measurements",
                   "influxdb.catalog.show", "SHOW", false, true)) return false;
  if (!ExpectRoute(parse, "influxdb", "show servers",
                   "influxdb.admin.show_servers", "SHOW")) return false;
  if (!CompatibilityWorkerSessionChecks("InfluxDB", Profile(), "influxdb")) return false;
  if (!ExpectOwnUnsupported(parse, "influxdb", "unsupported_influxdb_command",
                            "INFLUXDB.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool MilvusChecks() {
  using namespace scratchbird::parser::milvus;
  if (!Expect(DatatypeSurfaces().size() == 6,
              "Milvus datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 6,
              "Milvus builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 9,
              "Milvus catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 10,
              "Milvus diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(MilvusPackageIdentityJson(), "\"function_api_rows\":72") &&
                  Contains(MilvusPackageIdentityJson(), "\"policy_blocked_rows\":3") &&
                  Contains(MilvusPackageIdentityJson(), "\"trusted_udr_registration_rows\":10") &&
                  Contains(MilvusPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "Milvus package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "milvus", "search collection accounts vector [0.1,0.2] topk 10",
                   "milvus.query.search", "SEARCH")) return false;
  if (!ExpectRoute(parse, "milvus", "create collection accounts id int64 vector float_vector dim 128",
                   "milvus.collection.create", "CREATE_COLLECTION")) return false;
  if (!ExpectRoute(parse, "milvus", "create index accounts vector hnsw",
                   "milvus.index.create", "CREATE_INDEX", true)) return false;
  if (!ExpectRoute(parse, "milvus", "describe collection accounts",
                   "milvus.collection.describe", "DESCRIBE", false, true)) return false;
  if (!ExpectRoute(parse, "milvus", "transfer_replica collection accounts",
                   "milvus.admin.transfer_replica", "TRANSFER_REPLICA")) return false;
  if (!CompatibilityWorkerSessionChecks("Milvus", Profile(), "milvus")) return false;
  if (!ExpectOwnUnsupported(parse, "milvus", "unsupported_milvus_command",
                            "MILVUS.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool DoltChecks() {
  using namespace scratchbird::parser::dolt;
  if (!Expect(DatatypeSurfaces().size() == 10,
              "Dolt datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 9,
              "Dolt builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 10,
              "Dolt catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 10,
              "Dolt diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(DoltPackageIdentityJson(), "\"function_api_rows\":84") &&
                  Contains(DoltPackageIdentityJson(), "\"policy_blocked_rows\":0") &&
                  Contains(DoltPackageIdentityJson(), "\"trusted_udr_registration_rows\":15") &&
                  Contains(DoltPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "Dolt package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "dolt", "select * from dolt_log",
                   "dolt.version.log", "SELECT", false, true)) return false;
  if (!ExpectRoute(parse, "dolt", "select * from dolt_diff",
                   "dolt.version.diff", "SELECT", false, true)) return false;
  if (!ExpectRoute(parse, "dolt", "select * from dolt_status",
                   "dolt.version.status", "SELECT", false, true)) return false;
  if (!ExpectRoute(parse, "dolt", "select * from dolt_branches",
                   "dolt.version.branches", "SELECT", false, true)) return false;
  if (!ExpectRoute(parse, "dolt", "call dolt_commit('-Am','msg')",
                   "dolt.version.commit", "CALL", true)) return false;
  if (!ExpectRoute(parse, "dolt", "select dolt_commit('-Am','msg')",
                   "dolt.version.commit", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "dolt", "call dolt_branch('feature')",
                   "dolt.version.branch", "CALL", true)) return false;
  if (!ExpectRoute(parse, "dolt", "select dolt_branch('feature')",
                   "dolt.version.branch", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "dolt", "call dolt_checkout('feature')",
                   "dolt.version.checkout", "CALL", true)) return false;
  if (!ExpectRoute(parse, "dolt", "select dolt_checkout('feature')",
                   "dolt.version.checkout", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "dolt", "call dolt_merge('feature')",
                   "dolt.version.merge", "CALL", true)) return false;
  if (!ExpectRoute(parse, "dolt", "select dolt_merge('feature')",
                   "dolt.version.merge", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "dolt", "call dolt_stash('push')",
                   "dolt.version.stash", "CALL", true)) return false;
  if (!ExpectRoute(parse, "dolt", "select dolt_stash('push')",
                   "dolt.version.stash", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "dolt", "push origin main",
                   "dolt.remote.push", "PUSH", true)) return false;
  if (!ExpectRoute(parse, "dolt", "pull origin main",
                   "dolt.remote.pull", "PULL", true)) return false;
  if (!ExpectRoute(parse, "dolt", "call dolt_fetch('origin')",
                   "dolt.remote.fetch_sql", "CALL", true)) return false;
  if (!ExpectRoute(parse, "dolt", "select dolt_fetch('origin')",
                   "dolt.remote.fetch_sql", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "dolt", "call dolt_push('origin','main')",
                   "dolt.remote.push_sql", "CALL", true)) return false;
  if (!ExpectRoute(parse, "dolt", "select dolt_push('origin','main')",
                   "dolt.remote.push_sql", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "dolt", "call dolt_pull('origin','main')",
                   "dolt.remote.pull_sql", "CALL", true)) return false;
  if (!ExpectRoute(parse, "dolt", "select dolt_pull('origin','main')",
                   "dolt.remote.pull_sql", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "dolt", "select dolt_commit_name from t",
                   "dolt.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "dolt", "select dolt_fetch_name from t",
                   "dolt.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "dolt", "create table dolt_commit_name (id int)",
                   "dolt.ddl.create", "CREATE_TABLE", false, false)) return false;
  if (!ExpectRoute(parse, "dolt", "select 'dolt_commit(''msg'')'",
                   "dolt.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "dolt", "select 1 -- dolt_fetch('origin')",
                   "dolt.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "dolt", "select * from dolt_log_name",
                   "dolt.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "dolt", "select * from dolt_diff_name",
                   "dolt.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "dolt", "select * from dolt_status_name",
                   "dolt.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "dolt", "select * from dolt_branches_name",
                   "dolt.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "dolt", "select dolt_log from t",
                   "dolt.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "dolt", "select dolt_diff from t",
                   "dolt.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "dolt", "select dolt_status from t",
                   "dolt.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "dolt", "select dolt_branches from t",
                   "dolt.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "dolt", "select * from app.dolt_log",
                   "dolt.version.log", "SELECT", false, true)) return false;
  if (!ExpectRoute(parse, "dolt", "select * from t join dolt_status on true",
                   "dolt.version.status", "SELECT", false, true)) return false;
  if (!ExpectRoute(parse, "dolt", "insert into t values (1)",
                   "dolt.dml.insert", "INSERT")) return false;
  if (!CompatibilityWorkerSessionChecks("Dolt", Profile(), "dolt")) return false;
  if (!ExpectOwnUnsupported(parse, "dolt", "unsupported_dolt_command",
                            "DOLT.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool ApacheIgniteChecks() {
  using namespace scratchbird::parser::apache_ignite;
  if (!Expect(DatatypeSurfaces().size() == 10,
              "Apache Ignite datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 10,
              "Apache Ignite builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 10,
              "Apache Ignite catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 10,
              "Apache Ignite diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(ApacheIgnitePackageIdentityJson(), "\"function_api_rows\":92") &&
                  Contains(ApacheIgnitePackageIdentityJson(), "\"policy_blocked_rows\":6") &&
                  Contains(ApacheIgnitePackageIdentityJson(), "\"trusted_udr_registration_rows\":8") &&
                  Contains(ApacheIgnitePackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "Apache Ignite package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "apache_ignite", "select * from City",
                   "apache_ignite.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "apache_ignite",
                   "create table City (id int primary key)",
                   "apache_ignite.ddl.create_table", "CREATE_TABLE")) return false;
  if (!ExpectRoute(parse, "apache_ignite", "create cache CityCache",
                   "apache_ignite.cache.create", "CREATE_CACHE", true)) return false;
  if (!ExpectRoute(parse, "apache_ignite", "cache put CityCache key value",
                   "apache_ignite.cache.put", "CACHE")) return false;
  if (!ExpectRoute(parse, "apache_ignite", "scan CityCache",
                   "apache_ignite.cache.scan", "SCAN", true)) return false;
  if (!ExpectRoute(parse, "apache_ignite", "set streaming on",
                   "apache_ignite.session.streaming", "SET", true)) return false;
  if (!ExpectRoute(parse, "apache_ignite", "control.sh --baseline",
                   "apache_ignite.admin.control_script", "CONTROL",
                   false, false, false, true)) return false;
  if (!CompatibilityWorkerSessionChecks("Apache Ignite", Profile(), "apache_ignite")) return false;
  if (!ExpectOwnUnsupported(parse, "apache_ignite", "unsupported_apache_ignite_command",
                            "APACHE_IGNITE.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool TikvChecks() {
  using namespace scratchbird::parser::tikv;
  if (!Expect(DatatypeSurfaces().size() == 8,
              "TiKV datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 8,
              "TiKV builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 8,
              "TiKV catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 10,
              "TiKV diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(TikvPackageIdentityJson(), "\"function_api_rows\":74") &&
                  Contains(TikvPackageIdentityJson(), "\"policy_blocked_rows\":5") &&
                  Contains(TikvPackageIdentityJson(), "\"trusted_udr_registration_rows\":4") &&
                  Contains(TikvPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "TiKV package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "tikv", "RAW_GET account:1",
                   "tikv.raw.get", "RAW_GET")) return false;
  if (!ExpectRoute(parse, "tikv", "RAW_PUT account:1 active",
                   "tikv.raw.put", "RAW_PUT")) return false;
  if (!ExpectRoute(parse, "tikv", "TXN_PREWRITE account:1 active",
                   "tikv.txn.prewrite", "TXN_PREWRITE", true)) return false;
  if (!ExpectRoute(parse, "tikv", "COPROCESSOR table_scan accounts",
                   "tikv.coprocessor.request", "COPROCESSOR", true)) return false;
  if (!ExpectRoute(parse, "tikv", "REGION_INFO 1",
                   "tikv.catalog.region_info", "REGION_INFO", false, true)) return false;
  if (!ExpectRoute(parse, "tikv", "SPLIT_REGION 1",
                   "tikv.admin.split_region", "SPLIT_REGION")) return false;
  if (!CompatibilityWorkerSessionChecks("TiKV", Profile(), "tikv")) return false;
  if (!ExpectOwnUnsupported(parse, "tikv", "unsupported_tikv_command",
                            "TIKV.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool FoundationdbChecks() {
  using namespace scratchbird::parser::foundationdb;
  if (!Expect(DatatypeSurfaces().size() == 8,
              "FoundationDB datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 9,
              "FoundationDB builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 9,
              "FoundationDB catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 10,
              "FoundationDB diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(FoundationdbPackageIdentityJson(), "\"function_api_rows\":88") &&
                  Contains(FoundationdbPackageIdentityJson(), "\"policy_blocked_rows\":6") &&
                  Contains(FoundationdbPackageIdentityJson(), "\"trusted_udr_registration_rows\":9") &&
                  Contains(FoundationdbPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "FoundationDB package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "foundationdb", "GET account:1",
                   "foundationdb.kv.get", "GET")) return false;
  if (!ExpectRoute(parse, "foundationdb", "GET_RANGE accounts begin end",
                   "foundationdb.kv.get_range", "GET_RANGE")) return false;
  if (!ExpectRoute(parse, "foundationdb", "SET account:1 active",
                   "foundationdb.kv.set", "SET")) return false;
  if (!ExpectRoute(parse, "foundationdb", "ATOMIC_OP ADD counter 1",
                   "foundationdb.kv.atomic_op", "ATOMIC_OP", true)) return false;
  if (!ExpectRoute(parse, "foundationdb", "DIRECTORY_CREATE app users",
                   "foundationdb.directory.create", "DIRECTORY_CREATE", true)) return false;
  if (!ExpectRoute(parse, "foundationdb", "STATUS JSON",
                   "foundationdb.catalog.status", "STATUS", false, true)) return false;
  if (!ExpectRoute(parse, "foundationdb", "CONFIGURE new single memory",
                   "foundationdb.admin.configure", "CONFIGURE")) return false;
  if (!CompatibilityWorkerSessionChecks("FoundationDB", Profile(), "foundationdb")) return false;
  if (!ExpectOwnUnsupported(parse, "foundationdb", "unsupported_foundationdb_command",
                            "FOUNDATIONDB.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool ImmudbChecks() {
  using namespace scratchbird::parser::immudb;
  if (!Expect(DatatypeSurfaces().size() == 8,
              "immudb datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 8,
              "immudb builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 7,
              "immudb catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 10,
              "immudb diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(ImmudbPackageIdentityJson(), "\"function_api_rows\":7") &&
                  Contains(ImmudbPackageIdentityJson(), "\"catalog_projection_only_rows\":7") &&
                  Contains(ImmudbPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "immudb package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "immudb", "VERIFIED_GET account:1",
                   "immudb.kv.verified_get", "VERIFIED_GET", true)) return false;
  if (!ExpectRoute(parse, "immudb", "VERIFIED_SET account:1 active",
                   "immudb.kv.verified_set", "VERIFIED_SET", true)) return false;
  if (!ExpectRoute(parse, "immudb", "UPSERT INTO accounts VALUES (1, 'Ada')",
                   "immudb.dml.upsert", "UPSERT")) return false;
  if (!ExpectRoute(parse, "immudb", "SHOW DATABASES",
                   "immudb.catalog.show_databases", "SHOW", false, true)) return false;
  if (!ExpectRoute(parse, "immudb", "select databases()",
                   "immudb.catalog.databases", "SELECT", false, true)) return false;
  if (!ExpectRoute(parse, "immudb", "select tables()",
                   "immudb.catalog.tables", "SELECT", false, true)) return false;
  if (!ExpectRoute(parse, "immudb", "select columns()",
                   "immudb.catalog.columns", "SELECT", false, true)) return false;
  if (!ExpectRoute(parse, "immudb", "select indexes()",
                   "immudb.catalog.indexes", "SELECT", false, true)) return false;
  if (!ExpectRoute(parse, "immudb", "select databases_name from t",
                   "immudb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "immudb", "create table databases_name (id integer)",
                   "immudb.ddl.create_table", "CREATE_TABLE")) return false;
  if (!ExpectRoute(parse, "immudb", "select 'databases()'",
                   "immudb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "immudb", "select 1 -- databases()",
                   "immudb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "immudb", "select indexes_value from t",
                   "immudb.query.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "immudb", "DUMP database to '/tmp/x'",
                   "immudb.backup.dump", "DUMP",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "immudb", "REPLICATION status",
                   "immudb.replication.admin", "REPLICATION", true)) return false;
  if (!ExpectRoute(parse, "immudb", "REPLICATE tx 42",
                   "immudb.replication.apply", "REPLICATE", true)) return false;
  if (!CompatibilityWorkerSessionChecks("immudb", Profile(), "immudb")) return false;
  if (!ExpectOwnUnsupported(parse, "immudb", "unsupported_immudb_command",
                            "IMMUDB.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

bool XtdbChecks() {
  using namespace scratchbird::parser::xtdb;
  if (!Expect(DatatypeSurfaces().size() == 9,
              "XTDB datatype surface count mismatch")) return false;
  if (!Expect(BuiltinFunctionSurfaces().size() == 9,
              "XTDB builtin surface count mismatch")) return false;
  if (!Expect(CatalogOverlaySurfaces().size() == 10,
              "XTDB catalog surface count mismatch")) return false;
  if (!Expect(DiagnosticSurfaces().size() == 10,
              "XTDB diagnostic surface count mismatch")) return false;
  if (!Expect(Contains(XtdbPackageIdentityJson(), "\"function_api_rows\":9") &&
                  Contains(XtdbPackageIdentityJson(), "\"compatibility_alias_rows\":5") &&
                  Contains(XtdbPackageIdentityJson(), "\"catalog_projection_only_rows\":4") &&
                  Contains(XtdbPackageIdentityJson(), "\"standalone_dialect_package\":true"),
              "XTDB package identity mismatch")) return false;

  auto parse = [](std::string_view sql) { return ParseStatement(sql); };
  if (!ExpectRoute(parse, "xtdb", "XTDB_Q [:find ?e :where [?e :name \"Ada\"]]",
                   "xtdb.datalog.query", "XTDB_Q", true)) return false;
  if (!ExpectRoute(parse, "xtdb", "XTDB_SUBMIT_TX [{:xt/id :account/1 :name \"Ada\"}]",
                   "xtdb.entity.submit_tx", "XTDB_SUBMIT_TX", true)) return false;
  if (!ExpectRoute(parse, "xtdb", "select * from xt.live_tables",
                   "xtdb.sql.select", "SELECT")) return false;
  if (!ExpectRoute(parse, "xtdb", "select * from docs for valid_time as of now",
                   "xtdb.time.valid_time", "SELECT", true)) return false;
  if (!ExpectRoute(parse, "xtdb", "XTDB_MODULES",
                   "xtdb.catalog.modules", "XTDB_MODULES", false, true)) return false;
  if (!ExpectRoute(parse, "xtdb", "MODULES CONFIGURATION s3",
                   "xtdb.modules.configuration", "MODULES",
                   false, false, false, true)) return false;
  if (!ExpectRoute(parse, "xtdb", "CLUSTER status",
                   "xtdb.cluster.control", "CLUSTER")) return false;
  if (!CompatibilityWorkerSessionChecks("XTDB", Profile(), "xtdb")) return false;
  if (!ExpectOwnUnsupported(parse, "xtdb", "unsupported_xtdb_command",
                            "XTDB.PARSE.UNSUPPORTED_SURFACE")) return false;
  return true;
}

} // namespace

int main() {
  if (!FirebirdChecks()) return EXIT_FAILURE;
  if (!MysqlChecks()) return EXIT_FAILURE;
  if (!PostgresqlChecks()) return EXIT_FAILURE;
  if (!SqliteChecks()) return EXIT_FAILURE;
  if (!MariadbChecks()) return EXIT_FAILURE;
  if (!DuckdbChecks()) return EXIT_FAILURE;
  if (!ClickhouseChecks()) return EXIT_FAILURE;
  if (!TidbChecks()) return EXIT_FAILURE;
  if (!VitessChecks()) return EXIT_FAILURE;
  if (!CockroachdbChecks()) return EXIT_FAILURE;
  if (!YugabytedbChecks()) return EXIT_FAILURE;
  if (!CassandraChecks()) return EXIT_FAILURE;
  if (!MongodbChecks()) return EXIT_FAILURE;
  if (!RedisChecks()) return EXIT_FAILURE;
  if (!OpensearchSqlPplChecks()) return EXIT_FAILURE;
  if (!OpensearchChecks()) return EXIT_FAILURE;
  if (!Neo4jChecks()) return EXIT_FAILURE;
  if (!InfluxdbChecks()) return EXIT_FAILURE;
  if (!MilvusChecks()) return EXIT_FAILURE;
  if (!DoltChecks()) return EXIT_FAILURE;
  if (!ApacheIgniteChecks()) return EXIT_FAILURE;
  if (!TikvChecks()) return EXIT_FAILURE;
  if (!FoundationdbChecks()) return EXIT_FAILURE;
  if (!ImmudbChecks()) return EXIT_FAILURE;
  if (!XtdbChecks()) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
