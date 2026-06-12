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

constexpr std::string_view kTrustedContext =
    "engine_context=trusted;resolver=uuid";
constexpr std::string_view kFunctionalEvidenceKey =
    "\"procedural_functional_encoding_source_span_uuid_binding_evidence\":{";

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

bool ExpectPositiveCounter(std::string_view json,
                           std::string_view field,
                           std::string_view label) {
  const auto prefix = "\"" + std::string(field) + "\":";
  bool ok = true;
  ok &= Expect(Contains(json, prefix),
               std::string(label) + " missing counter " + std::string(field));
  ok &= Expect(!Contains(json, prefix + "0,") &&
                   !Contains(json, prefix + "0}"),
               std::string(label) + " has zero counter " + std::string(field));
  return ok;
}

bool ExpectNoMarkers(std::string_view payload,
                     std::initializer_list<std::string_view> markers,
                     std::string_view label) {
  bool ok = true;
  for (const auto marker : markers) {
    ok &= Expect(!Contains(payload, marker),
                 std::string(label) + " leaked source/body marker: " +
                     std::string(marker));
  }
  ok &= Expect(!Contains(payload, "mysql_lts") &&
                   !Contains(payload, "MYSQL_LTS"),
               std::string(label) +
                   " emitted mysql_lts runtime dialect evidence");
  return ok;
}

bool ExpectSourceRetention(std::string_view payload, std::string_view label) {
  const bool firebird_payload = Contains(payload, "\"dialect\":\"firebird\"");
  bool ok = true;
  ok &= Expect(Contains(payload, "\"procedural_body_source_retention_evidence\":{"),
               std::string(label) + " missing source retention evidence");
  ok &= ExpectBool(payload, "raw_sql_body_embedded_in_sblr_envelope", false,
                   label);
  ok &= ExpectBool(payload, "body_text_redacted_from_parser_evidence", true,
                   label);
  ok &= ExpectBool(payload, "uuid_binding_required", true, label);
  ok &= ExpectBool(payload, "compatibility_sql_executed", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_bound_sblr_body_instruction_stream",
                   firebird_payload, label);
  ok &= ExpectBool(payload, "uuid_dependency_bindings_bound",
                   firebird_payload, label);
  ok &= ExpectField(payload, "body_lowering_status",
                    firebird_payload
                        ? "parser_bound_sblr_instruction_stream_encoded"
                        : "lowering_pending",
                    label);
  return ok;
}

bool ExpectFunctionalEncoding(std::string_view payload,
                              std::string_view dialect,
                              std::string_view label) {
  const bool firebird_payload = dialect == "firebird";
  bool ok = true;
  ok &= ExpectField(payload, "dialect", dialect, label);
  ok &= Expect(Contains(payload, kFunctionalEvidenceKey),
               std::string(label) + " missing functional encoding evidence");
  ok &= ExpectField(
      payload, "evidence_contract",
      "compatibility_procedural_functional_encoding_source_span_uuid_binding.v1",
      label);
  ok &= ExpectBool(payload, "compatibility_cst_materialized", true, label);
  ok &= ExpectBool(payload, "compatibility_ast_materialized", true, label);
  ok &= ExpectBool(payload, "compatibility_bound_ast_materialized", true, label);
  ok &= ExpectBool(payload, "source_span_map_present", true, label);
  ok &= Expect(Contains(payload, "\"source_span_count\":"),
               std::string(label) + " missing source span count");
  ok &= ExpectBool(payload, "source_text_redacted_from_parser_evidence", true,
                   label);
  ok &= ExpectBool(payload, "sblr_evidence_includes_source_text", false, label);
  ok &= ExpectField(payload, "routine_body_segmentation",
                    "header_body_span_metadata_only", label);
  ok &= ExpectBool(payload, "header_span_metadata_present", true, label);
  ok &= ExpectBool(payload, "body_span_metadata_present", true, label);
  ok &= ExpectPositiveCounter(payload, "header_source_span_count", label);
  ok &= ExpectPositiveCounter(payload, "body_source_span_count", label);
  ok &= ExpectBool(payload, "body_text_included", false, label);
  ok &= ExpectBool(payload, "parser_bound_sblr_body_instruction_stream",
                   firebird_payload, label);
  ok &= ExpectBool(payload, "uuid_bound_ast_required", true, label);
  ok &= ExpectBool(payload, "uuid_dependency_bindings_required", true,
                   label);
  ok &= ExpectBool(payload, "uuid_dependency_bindings_bound",
                   firebird_payload, label);
  ok &= ExpectField(payload, "uuid_binding_authority",
                    "scratchbird_engine_catalog", label);
  ok &= ExpectBool(payload, "parser_uuid_authority", false, label);
  ok &= ExpectField(payload, "dependency_resolution_authority",
                    "scratchbird_engine_catalog", label);
  ok &= ExpectBool(payload, "parser_dependency_authority", false, label);
  ok &= ExpectBool(payload, "executable_sblr_lowering_required", true, label);
  ok &= ExpectField(payload, "executable_sblr_lowering_status",
                    firebird_payload
                        ? "parser_bound_sblr_instruction_stream_encoded"
                        : "pending",
                    label);
  ok &= ExpectBool(payload, "jit_readiness_required", true, label);
  ok &= ExpectField(payload, "jit_readiness_status",
                    firebird_payload
                        ? "parser_bound_sblr_requires_runtime_codegen_proof"
                        : "pending",
                    label);
  ok &= ExpectBool(payload, "aot_readiness_required", true, label);
  ok &= ExpectField(payload, "aot_readiness_status",
                    firebird_payload
                        ? "parser_bound_sblr_requires_runtime_codegen_proof"
                        : "pending",
                    label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_sequence_value_authority", false, label);
  ok &= ExpectBool(payload, "parser_source_execution_authority", false, label);
  ok &= ExpectBool(payload, "compatibility_sql_executed", false, label);
  ok &= ExpectField(payload, "original_source_usage",
                    "catalog_audit_reference_only", label);
  ok &= ExpectBool(payload, "original_source_executed", false, label);
  ok &= ExpectBool(payload, "catalog_source_reference_execute_allowed", false,
                   label);
  if (firebird_payload) {
    ok &= Expect(Contains(payload,
                          "\"firebird_psql_functional_encoding_evidence\":{"),
                 std::string(label) +
                     " missing Firebird PSQL functional encoding evidence");
    ok &= ExpectField(payload, "evidence_contract",
                      "firebird_psql_functional_encoding.v1", label);
    ok &= ExpectField(payload, "functional_encoding_status",
                      "firebird_psql_parser_bound_sblr_encoded", label);
    ok &= ExpectField(payload, "runtime_equivalence_status",
                      "pending_compatibility_native_psql_replay", label);
  }
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready",
                    label);
  return ok;
}

bool ExpectPayload(std::string_view payload,
                   std::string_view dialect,
                   std::initializer_list<std::string_view> redacted_markers,
                   std::string_view label) {
  bool ok = true;
  ok &= ExpectSourceRetention(payload, label);
  ok &= ExpectFunctionalEncoding(payload, dialect, label);
  ok &= ExpectNoMarkers(payload, redacted_markers, label);
  return ok;
}

template <typename Result>
bool ExpectDirectResult(const Result& result,
                        std::string_view dialect,
                        std::initializer_list<std::string_view> redacted_markers,
                        std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " direct parse failed");
  ok &= ExpectBool(result.sblr_envelope, "sql_text_included", false, label);
  ok &= ExpectBool(result.sblr_envelope, "reference_engine_sql_executed", false,
                   label);
  ok &= ExpectPayload(result.sblr_envelope, dialect, redacted_markers, label);
  ok &= ExpectPayload(result.parser_evidence_json, dialect, redacted_markers,
                      std::string(label) + " parser evidence");
  return ok;
}

template <typename UdrResult>
bool ExpectUdrResult(const UdrResult& result,
                     std::string_view dialect,
                     std::initializer_list<std::string_view> redacted_markers,
                     std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= Expect(Contains(result.payload, "\"envelope\":\"SBLRExecutionEnvelope.v3\""),
               std::string(label) + " missing SBLR envelope");
  ok &= ExpectBool(result.payload, "sql_text_included", false, label);
  ok &= ExpectBool(result.payload, "reference_engine_sql_executed", false, label);
  ok &= ExpectPayload(result.payload, dialect, redacted_markers, label);
  return ok;
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  bool ok = true;
  const std::string_view create_proc =
      "create procedure fb_fe_proc as begin post_event 'fb_fe_proc_secret'; end";
  ok &= ExpectDirectResult(ParseStatement(create_proc), "firebird",
                           {"fb_fe_proc", "fb_fe_proc_secret", "post_event"},
                           "firebird direct procedure");
  ok &= ExpectUdrResult(sbu_firebird_parse_to_sblr(create_proc,
                                                   kTrustedContext),
                        "firebird",
                        {"fb_fe_proc", "fb_fe_proc_secret", "post_event"},
                        "firebird UDR procedure");
  const std::string_view alter_proc =
      "alter procedure fb_fe_proc as begin post_event 'fb_fe_proc_alter_secret'; end";
  ok &= ExpectDirectResult(
      ParseStatement(alter_proc), "firebird",
      {"fb_fe_proc", "fb_fe_proc_alter_secret", "post_event"},
      "firebird direct alter procedure");
  ok &= ExpectUdrResult(
      sbu_firebird_parse_to_sblr(alter_proc, kTrustedContext), "firebird",
      {"fb_fe_proc", "fb_fe_proc_alter_secret", "post_event"},
      "firebird UDR alter procedure");
  const std::string_view recreate_proc =
      "recreate procedure fb_fe_recreate as begin post_event 'fb_fe_recreate_secret'; end";
  ok &= ExpectDirectResult(
      ParseStatement(recreate_proc), "firebird",
      {"fb_fe_recreate", "fb_fe_recreate_secret", "post_event"},
      "firebird direct recreate procedure");
  ok &= ExpectUdrResult(
      sbu_firebird_parse_to_sblr(recreate_proc, kTrustedContext), "firebird",
      {"fb_fe_recreate", "fb_fe_recreate_secret", "post_event"},
      "firebird UDR recreate procedure");
  const std::string_view create_fn =
      "create function fb_fe_fn returns varchar(20) as begin return 'fb_fe_fn_secret'; end";
  ok &= ExpectDirectResult(ParseStatement(create_fn), "firebird",
                           {"fb_fe_fn", "fb_fe_fn_secret"},
                           "firebird direct function");
  ok &= ExpectUdrResult(sbu_firebird_parse_to_sblr(create_fn,
                                                   kTrustedContext),
                        "firebird", {"fb_fe_fn", "fb_fe_fn_secret"},
                        "firebird UDR function");
  const std::string_view alter_fn =
      "alter function fb_fe_fn returns varchar(20) as begin return 'fb_fe_fn_alter_secret'; end";
  ok &= ExpectDirectResult(ParseStatement(alter_fn), "firebird",
                           {"fb_fe_fn", "fb_fe_fn_alter_secret"},
                           "firebird direct alter function");
  ok &= ExpectUdrResult(sbu_firebird_parse_to_sblr(alter_fn,
                                                   kTrustedContext),
                        "firebird", {"fb_fe_fn", "fb_fe_fn_alter_secret"},
                        "firebird UDR alter function");
  const std::string_view create_trigger =
      "create trigger fb_fe_tr for t before insert as begin post_event 'fb_fe_tr_secret'; end";
  ok &= ExpectDirectResult(
      ParseStatement(create_trigger), "firebird",
      {"fb_fe_tr", "fb_fe_tr_secret", "post_event"},
      "firebird direct trigger");
  ok &= ExpectUdrResult(
      sbu_firebird_parse_to_sblr(create_trigger, kTrustedContext), "firebird",
      {"fb_fe_tr", "fb_fe_tr_secret", "post_event"},
      "firebird UDR trigger");
  const std::string_view alter_trigger =
      "alter trigger fb_fe_tr active as begin post_event 'fb_fe_tr_alter_secret'; end";
  ok &= ExpectDirectResult(
      ParseStatement(alter_trigger), "firebird",
      {"fb_fe_tr", "fb_fe_tr_alter_secret", "post_event"},
      "firebird direct alter trigger");
  ok &= ExpectUdrResult(
      sbu_firebird_parse_to_sblr(alter_trigger, kTrustedContext), "firebird",
      {"fb_fe_tr", "fb_fe_tr_alter_secret", "post_event"},
      "firebird UDR alter trigger");
  const std::string_view create_package_body =
      "create package body fb_fe_pkg as begin procedure p as begin post_event 'fb_fe_pkg_secret'; end end";
  ok &= ExpectDirectResult(
      ParseStatement(create_package_body), "firebird",
      {"fb_fe_pkg", "fb_fe_pkg_secret", "post_event"},
      "firebird direct package body");
  ok &= ExpectUdrResult(
      sbu_firebird_parse_to_sblr(create_package_body, kTrustedContext),
      "firebird", {"fb_fe_pkg", "fb_fe_pkg_secret", "post_event"},
      "firebird UDR package body");
  const std::string_view recreate_package_body =
      "recreate package body fb_fe_pkg as begin procedure p as begin post_event 'fb_fe_pkg_recreate_secret'; end end";
  ok &= ExpectDirectResult(
      ParseStatement(recreate_package_body), "firebird",
      {"fb_fe_pkg", "fb_fe_pkg_recreate_secret", "post_event"},
      "firebird direct recreate package body");
  ok &= ExpectUdrResult(
      sbu_firebird_parse_to_sblr(recreate_package_body, kTrustedContext),
      "firebird", {"fb_fe_pkg", "fb_fe_pkg_recreate_secret", "post_event"},
      "firebird UDR recreate package body");
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  bool ok = true;
  ok &= ExpectDirectResult(
      ParseStatement("create procedure mysql_fe_proc() begin select 'mysql_fe_proc_secret'; end"),
      "mysql", {"mysql_fe_proc", "mysql_fe_proc_secret"},
      "mysql direct procedure");
  ok &= ExpectUdrResult(
      sbu_mysql_parse_to_sblr(
          "create procedure mysql_fe_udr_proc() begin select 'mysql_fe_udr_proc_secret'; end",
          kTrustedContext),
      "mysql", {"mysql_fe_udr_proc", "mysql_fe_udr_proc_secret"},
      "mysql UDR procedure");
  ok &= ExpectDirectResult(
      ParseStatement("create function mysql_fe_fn() returns varchar(20) begin return 'mysql_fe_fn_secret'; end"),
      "mysql", {"mysql_fe_fn", "mysql_fe_fn_secret"},
      "mysql direct function");
  ok &= ExpectUdrResult(
      sbu_mysql_parse_to_sblr(
          "create function mysql_fe_udr_fn() returns varchar(20) begin return 'mysql_fe_udr_fn_secret'; end",
          kTrustedContext),
      "mysql", {"mysql_fe_udr_fn", "mysql_fe_udr_fn_secret"},
      "mysql UDR function");
  ok &= ExpectDirectResult(
      ParseStatement("create trigger mysql_fe_tr before insert on t for each row set @mysql_fe_tr_secret = 1"),
      "mysql", {"mysql_fe_tr", "mysql_fe_tr_secret"},
      "mysql direct trigger");
  ok &= ExpectUdrResult(
      sbu_mysql_parse_to_sblr(
          "create trigger mysql_fe_udr_tr before insert on t for each row set @mysql_fe_udr_tr_secret = 1",
          kTrustedContext),
      "mysql", {"mysql_fe_udr_tr", "mysql_fe_udr_tr_secret"},
      "mysql UDR trigger");
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::
      sbu_postgresql_parse_to_sblr;
  bool ok = true;
  ok &= ExpectDirectResult(
      ParseStatement("create procedure pg_fe_proc() language sql as 'select 1 /* pg_fe_proc_secret */'"),
      "postgresql", {"pg_fe_proc", "pg_fe_proc_secret"},
      "postgresql direct procedure");
  ok &= ExpectUdrResult(
      sbu_postgresql_parse_to_sblr(
          "create procedure pg_fe_udr_proc() language sql as 'select 1 /* pg_fe_udr_proc_secret */'",
          kTrustedContext),
      "postgresql", {"pg_fe_udr_proc", "pg_fe_udr_proc_secret"},
      "postgresql UDR procedure");
  ok &= ExpectDirectResult(
      ParseStatement("create function pg_fe_fn() returns integer language sql as 'select 1 /* pg_fe_fn_secret */'"),
      "postgresql", {"pg_fe_fn", "pg_fe_fn_secret"},
      "postgresql direct function");
  ok &= ExpectUdrResult(
      sbu_postgresql_parse_to_sblr(
          "create function pg_fe_udr_fn() returns integer language sql as 'select 1 /* pg_fe_udr_fn_secret */'",
          kTrustedContext),
      "postgresql", {"pg_fe_udr_fn", "pg_fe_udr_fn_secret"},
      "postgresql UDR function");
  ok &= ExpectDirectResult(
      ParseStatement("create trigger pg_fe_tr before insert on t execute function pg_fe_fn()"),
      "postgresql", {"pg_fe_tr", "pg_fe_fn"},
      "postgresql direct trigger");
  ok &= ExpectUdrResult(
      sbu_postgresql_parse_to_sblr(
          "create trigger pg_fe_udr_tr before insert on t execute function pg_fe_udr_fn()",
          kTrustedContext),
      "postgresql", {"pg_fe_udr_tr", "pg_fe_udr_fn"},
      "postgresql UDR trigger");
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
