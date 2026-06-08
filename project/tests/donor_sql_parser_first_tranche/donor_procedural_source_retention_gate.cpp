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
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::string_view kTrustedContext =
    "engine_context=trusted;resolver=uuid";

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
                 std::string_view value) {
  return Expect(Contains(json, "\"" + std::string(field) + "\":\"" +
                                  std::string(value) + "\""),
                "missing source-retention field: " + std::string(field));
}

bool ExpectBool(std::string_view json,
                std::string_view field,
                bool value) {
  return Expect(Contains(json, "\"" + std::string(field) + "\":" +
                                  std::string(value ? "true" : "false")),
                "missing source-retention bool: " + std::string(field));
}

std::optional<std::size_t> ExtractUnsigned(std::string_view json,
                                           std::string_view field) {
  const auto prefix = "\"" + std::string(field) + "\":";
  const auto pos = json.find(prefix);
  if (pos == std::string_view::npos) return std::nullopt;
  std::size_t cursor = pos + prefix.size();
  std::size_t value = 0;
  bool saw_digit = false;
  while (cursor < json.size() && json[cursor] >= '0' && json[cursor] <= '9') {
    saw_digit = true;
    value = (value * 10) + static_cast<std::size_t>(json[cursor] - '0');
    ++cursor;
  }
  if (!saw_digit) return std::nullopt;
  return value;
}

std::optional<std::size_t> ExtractUnsignedAfter(std::string_view json,
                                                std::string_view marker,
                                                std::string_view field) {
  const auto marker_pos = json.find(marker);
  if (marker_pos == std::string_view::npos) return std::nullopt;
  return ExtractUnsigned(json.substr(marker_pos), field);
}

bool ExpectUnsigned(std::string_view json,
                    std::string_view field,
                    std::size_t value) {
  const auto actual = ExtractUnsigned(json, field);
  return Expect(actual.has_value() && *actual == value,
                "source-retention numeric field mismatch: " +
                    std::string(field));
}

bool ExpectPositiveUnsigned(std::string_view json, std::string_view field) {
  const auto actual = ExtractUnsigned(json, field);
  return Expect(actual.has_value() && *actual > 0,
                "missing positive source-retention numeric field: " +
                    std::string(field));
}

bool ExpectSourceRetentionEvidence(std::string_view json,
                                   std::string_view sql_text) {
  const bool firebird_payload = Contains(json, "\"dialect\":\"firebird\"");
  const auto normalized =
      scratchbird::parser::donor::NormalizeWhitespace(sql_text);
  const auto header_start =
      ExtractUnsignedAfter(json, "\"header_source_range\":{", "start_byte");
  const auto header_end =
      ExtractUnsignedAfter(json, "\"header_source_range\":{", "end_byte");
  const auto body_start =
      ExtractUnsignedAfter(json, "\"body_source_range\":{", "start_byte");
  const auto body_end =
      ExtractUnsignedAfter(json, "\"body_source_range\":{", "end_byte");
  const auto header_spans =
      ExtractUnsignedAfter(json, "\"header_source_range\":{", "source_span_count");
  const auto body_spans =
      ExtractUnsignedAfter(json, "\"body_source_range\":{", "source_span_count");
  bool ok = true;
  ok &= Expect(Contains(json, "\"procedural_body_source_retention_evidence\":{"),
               "missing procedural body source-retention evidence");
  ok &= ExpectField(json, "evidence_contract",
                    "donor_procedural_body_source_retention.v1");
  ok &= ExpectField(json, "source_retention_state",
                    "catalog_reference_audit_material");
  ok &= ExpectField(json, "source_retention_metadata_source",
                    "parser_derived_token_offsets");
  ok &= ExpectBool(json, "parser_derived_source_range_metadata", true);
  ok &= ExpectBool(json, "source_text_included", false);
  ok &= ExpectUnsigned(json, "source_byte_length", normalized.size());
  ok &= Expect(Contains(json, "\"source_hash_descriptor\":\"fnv1a64:"),
               "missing source hash descriptor");
  ok &= Expect(Contains(json, "\"header_source_range\":{\"start_byte\":0,"),
               "missing header source range metadata");
  ok &= Expect(Contains(json, "\"body_source_range\":{\"start_byte\":"),
               "missing body source range metadata");
  ok &= Expect(header_start.has_value() && *header_start == 0,
               "header source range must start at byte zero");
  ok &= Expect(header_end.has_value() && *header_end > 0,
               "header source range must have a positive end byte");
  ok &= Expect(body_start.has_value() && body_end.has_value(),
               "body source range must include start and end bytes");
  if (header_end.has_value() && body_start.has_value()) {
    ok &= Expect(*header_end == *body_start,
                 "header end byte must equal body start byte");
  }
  if (body_start.has_value() && body_end.has_value()) {
    ok &= Expect(*body_start > 0 && *body_start < *body_end,
                 "body source range must be non-empty and after the header");
    ok &= Expect(*body_end == normalized.size(),
                 "body source range must end at normalized source length");
  }
  ok &= Expect(header_spans.has_value() && *header_spans > 0,
               "header source range must include parser-derived spans");
  ok &= Expect(body_spans.has_value() && *body_spans > 0,
               "body source range must include parser-derived spans");
  ok &= ExpectBool(json, "catalog_source_reference_required", true);
  ok &= ExpectBool(json, "catalog_audit_material", true);
  ok &= ExpectField(json, "original_source_usage",
                    "audit_reference_only_not_runtime_authority");
  ok &= ExpectBool(json, "original_source_runtime_authority", false);
  ok &= ExpectBool(json, "raw_sql_body_embedded_in_sblr_envelope", false);
  ok &= ExpectBool(json, "body_text_redacted_from_parser_evidence", true);
  ok &= ExpectBool(json, "uuid_binding_required", true);
  ok &= ExpectField(json, "execution_authority", "scratchbird_engine_sblr");
  ok &= ExpectBool(json, "donor_sql_executed", false);
  ok &= ExpectBool(json, "parser_transaction_authority", false);
  ok &= ExpectBool(json, "parser_storage_authority", false);
  ok &= ExpectBool(json, "parser_execution_authority", false);
  ok &= ExpectBool(json, "parser_runtime_authority", false);
  ok &= ExpectBool(json, "parser_bound_sblr_body_instruction_stream",
                   firebird_payload);
  ok &= ExpectBool(json, "uuid_dependency_bindings_bound",
                   firebird_payload);
  ok &= ExpectField(json, "body_lowering_status",
                    firebird_payload
                        ? "parser_bound_sblr_instruction_stream_encoded"
                        : "lowering_pending");
  ok &= ExpectField(
      json, "compiled_sblr_status",
      firebird_payload
          ? "parser_bound_instruction_stream_present_runtime_compile_pending"
          : "pending");
  ok &= ExpectField(json, "runtime_executable_status", "pending");
  ok &= ExpectField(json, "runtime_storage_status", "pending");
  ok &= ExpectField(json, "catalog_persistence_status", "pending");
  ok &= ExpectField(json, "catalog_reopen_runtime_proof_status", "pending");
  ok &= ExpectField(json, "enterprise_readiness", "not_enterprise_ready");
  return ok;
}

template <typename ParseResult>
bool ExpectRoutineParse(std::string_view dialect,
                        std::string_view label,
                        std::string_view sql_text,
                        const ParseResult& result,
                        const std::vector<std::string_view>& leak_markers) {
  bool ok = true;
  const auto context = std::string(dialect) + " " + std::string(label);
  ok &= Expect(result.ok, context + " did not parse");
  ok &= Expect(Contains(result.sblr_envelope,
                        "\"descriptor_resolution\":\"uuid_required\""),
               context + " missing UUID descriptor resolution");
  ok &= Expect(Contains(result.sblr_envelope,
                        "\"engine_authority\":\"scratchbird\""),
               context + " missing ScratchBird engine authority");
  ok &= Expect(Contains(result.sblr_envelope,
                        "\"donor_engine_sql_executed\":false"),
               context + " permits donor SQL execution");
  ok &= Expect(Contains(result.sblr_envelope, "\"sql_text_included\":false"),
               context + " includes SQL text in SBLR envelope");
  ok &= Expect(Contains(result.parser_evidence_json,
                        "\"source_text_redacted\":true"),
               context + " parser evidence does not redact source text");
  ok &= Expect(Contains(result.parser_evidence_json,
                        "\"parser_transaction_finality_authority\":false"),
               context + " parser evidence grants transaction finality");
  ok &= Expect(Contains(result.parser_evidence_json,
                        "\"parser_storage_authority\":false"),
               context + " parser evidence grants storage authority");
  ok &= ExpectSourceRetentionEvidence(result.sblr_envelope, sql_text);
  ok &= ExpectSourceRetentionEvidence(result.parser_evidence_json, sql_text);
  for (const auto marker : leak_markers) {
    ok &= Expect(!Contains(result.sblr_envelope, marker),
                 context + " leaked body/source marker into SBLR envelope: " +
                     std::string(marker));
    ok &= Expect(!Contains(result.parser_evidence_json, marker),
                 context + " leaked body/source marker into parser evidence: " +
                     std::string(marker));
  }
  return ok;
}

template <typename UdrResult>
bool ExpectRoutineUdr(std::string_view dialect,
                      std::string_view label,
                      std::string_view sql_text,
                      const UdrResult& result,
                      const std::vector<std::string_view>& leak_markers) {
  bool ok = true;
  const auto context = std::string(dialect) + " " + std::string(label);
  ok &= Expect(result.ok, context + " UDR did not parse: " +
                             result.message_vector_json);
  ok &= Expect(Contains(result.payload, "\"envelope\":\"SBLRExecutionEnvelope.v3\""),
               context + " missing SBLR envelope");
  ok &= Expect(Contains(result.payload, "\"descriptor_resolution\":\"uuid_required\""),
               context + " missing UUID descriptor resolution");
  ok &= Expect(Contains(result.payload, "\"engine_authority\":\"scratchbird\""),
               context + " missing ScratchBird engine authority");
  ok &= Expect(Contains(result.payload, "\"donor_engine_sql_executed\":false"),
               context + " permits donor SQL execution");
  ok &= Expect(Contains(result.payload, "\"sql_text_included\":false"),
               context + " includes SQL text in UDR SBLR payload");
  ok &= ExpectSourceRetentionEvidence(result.payload, sql_text);
  for (const auto marker : leak_markers) {
    ok &= Expect(!Contains(result.payload, marker),
                 context + " leaked body/source marker into UDR payload: " +
                     std::string(marker));
  }
  return ok;
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  bool ok = true;
  const std::string_view proc =
      "create procedure retain_fb_proc as begin post_event 'fb_body_secret_proc'; end";
  ok &= ExpectRoutineParse("firebird", "create procedure", proc,
                           ParseStatement(proc),
                           {"retain_fb_proc", "fb_body_secret_proc",
                            "post_event"});
  ok &= ExpectRoutineUdr("firebird", "UDR create procedure", proc,
                         sbu_firebird_parse_to_sblr(proc, kTrustedContext),
                         {"retain_fb_proc", "fb_body_secret_proc",
                          "post_event"});
  const std::string_view alter_proc =
      "alter procedure retain_fb_proc as begin post_event 'fb_body_secret_alter_proc'; end";
  ok &= ExpectRoutineParse("firebird", "alter procedure", alter_proc,
                           ParseStatement(alter_proc),
                           {"retain_fb_proc", "fb_body_secret_alter_proc",
                            "post_event"});
  ok &= ExpectRoutineUdr("firebird", "UDR alter procedure", alter_proc,
                         sbu_firebird_parse_to_sblr(alter_proc,
                                                    kTrustedContext),
                         {"retain_fb_proc", "fb_body_secret_alter_proc",
                          "post_event"});
  const std::string_view fn =
      "create function retain_fb_fn returns integer as begin return 1; end";
  ok &= ExpectRoutineParse("firebird", "create function", fn,
                           ParseStatement(fn),
                           {"retain_fb_fn", "return 1"});
  const std::string_view alter_fn =
      "alter function retain_fb_fn returns integer as begin return 2; end";
  ok &= ExpectRoutineParse("firebird", "alter function", alter_fn,
                           ParseStatement(alter_fn),
                           {"retain_fb_fn", "return 2"});
  ok &= ExpectRoutineUdr("firebird", "UDR alter function", alter_fn,
                         sbu_firebird_parse_to_sblr(alter_fn,
                                                    kTrustedContext),
                         {"retain_fb_fn", "return 2"});
  const std::string_view tr =
      "create trigger retain_fb_tr for t before insert as begin post_event 'fb_body_secret_tr'; end";
  ok &= ExpectRoutineParse("firebird", "create trigger", tr,
                           ParseStatement(tr),
                           {"retain_fb_tr", "fb_body_secret_tr",
                            "post_event"});
  const std::string_view alter_tr =
      "alter trigger retain_fb_tr active as begin post_event 'fb_body_secret_alter_tr'; end";
  ok &= ExpectRoutineParse("firebird", "alter trigger", alter_tr,
                           ParseStatement(alter_tr),
                           {"retain_fb_tr", "fb_body_secret_alter_tr",
                            "post_event"});
  ok &= ExpectRoutineUdr("firebird", "UDR alter trigger", alter_tr,
                         sbu_firebird_parse_to_sblr(alter_tr,
                                                    kTrustedContext),
                         {"retain_fb_tr", "fb_body_secret_alter_tr",
                          "post_event"});
  const std::string_view pkg =
      "create package body retain_fb_pkg as begin procedure p as begin post_event 'fb_body_secret_pkg'; end end";
  ok &= ExpectRoutineParse("firebird", "create package body", pkg,
                           ParseStatement(pkg),
                           {"retain_fb_pkg", "fb_body_secret_pkg",
                            "post_event"});
  const std::string_view recreate_pkg =
      "recreate package body retain_fb_pkg as begin procedure p as begin post_event 'fb_body_secret_recreate_pkg'; end end";
  ok &= ExpectRoutineParse("firebird", "recreate package body", recreate_pkg,
                           ParseStatement(recreate_pkg),
                           {"retain_fb_pkg", "fb_body_secret_recreate_pkg",
                            "post_event"});
  ok &= ExpectRoutineUdr("firebird", "UDR recreate package body",
                         recreate_pkg,
                         sbu_firebird_parse_to_sblr(recreate_pkg,
                                                    kTrustedContext),
                         {"retain_fb_pkg", "fb_body_secret_recreate_pkg",
                          "post_event"});
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  bool ok = true;
  const std::string_view proc =
      "create procedure retain_mysql_proc() begin select 'mysql_body_secret_proc'; end";
  ok &= ExpectRoutineParse("mysql", "create procedure", proc,
                           ParseStatement(proc),
                           {"retain_mysql_proc", "mysql_body_secret_proc"});
  ok &= ExpectRoutineUdr("mysql", "UDR create procedure", proc,
                         sbu_mysql_parse_to_sblr(proc, kTrustedContext),
                         {"retain_mysql_proc", "mysql_body_secret_proc"});
  const std::string_view fn =
      "create function retain_mysql_fn() returns int begin return 1; end";
  ok &= ExpectRoutineParse("mysql", "create function", fn,
                           ParseStatement(fn),
                           {"retain_mysql_fn", "return 1"});
  const std::string_view tr =
      "create trigger retain_mysql_tr before insert on t for each row set @mysql_body_secret_tr = 1";
  ok &= ExpectRoutineParse("mysql", "create trigger", tr,
                           ParseStatement(tr),
                           {"retain_mysql_tr", "mysql_body_secret_tr"});
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::
      sbu_postgresql_parse_to_sblr;
  bool ok = true;
  const std::string_view fn =
      "create function retain_pg_fn() returns integer language sql as 'select 1 /* pg_body_secret_fn */'";
  ok &= ExpectRoutineParse("postgresql", "create function", fn,
                           ParseStatement(fn),
                           {"retain_pg_fn", "pg_body_secret_fn"});
  ok &= ExpectRoutineUdr("postgresql", "UDR create function", fn,
                         sbu_postgresql_parse_to_sblr(fn, kTrustedContext),
                         {"retain_pg_fn", "pg_body_secret_fn"});
  const std::string_view proc =
      "create procedure retain_pg_proc() language sql as 'select 1 /* pg_body_secret_proc */'";
  ok &= ExpectRoutineParse("postgresql", "create procedure", proc,
                           ParseStatement(proc),
                           {"retain_pg_proc", "pg_body_secret_proc"});
  const std::string_view tr =
      "create trigger retain_pg_tr before insert on t execute function retain_pg_fn()";
  ok &= ExpectRoutineParse("postgresql", "create trigger", tr,
                           ParseStatement(tr),
                           {"retain_pg_tr", "retain_pg_fn"});
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
