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
                 std::string(label) + " leaked source marker: " +
                     std::string(marker));
  }
  return ok;
}

bool ExpectCommonParserEvidence(std::string_view payload,
                                std::string_view dialect,
                                std::string_view label) {
  bool ok = true;
  ok &= ExpectField(payload, "dialect", dialect, label);
  ok &= ExpectBool(payload, "source_text_redacted", true, label);
  ok &= ExpectBool(payload, "descriptor_uuid_required", true, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
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
  ok &= Expect(Contains(payload, "\"parser_evidence\":{"),
               std::string(label) + " missing nested parser evidence");
  ok &= ExpectCommonParserEvidence(payload, dialect, label);
  ok &= Expect(Contains(payload, "\"enterprise_readiness_evidence\":{"),
               std::string(label) + " missing readiness truth evidence");
  ok &= ExpectField(payload, "completion_claim", "not_enterprise_ready", label);
  ok &= ExpectBool(payload, "enterprise_implemented_proven", false, label);
  ok &= ExpectField(payload, "procedural_body_encoding_status",
                    "route_and_descriptor_only_not_enterprise", label);
  ok &= ExpectField(payload, "datatype_exactness_status",
                    "surface_cataloged_exactness_proof_pending", label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= Expect(Contains(payload, "enterprise_implemented_proven\":false"),
               std::string(label) + " missing enterprise implementation proof");
  return ok;
}

bool ExpectDatatypeDescriptor(std::string_view payload,
                              std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"datatype_descriptor_evidence\":{"),
               std::string(label) + " missing datatype descriptor evidence");
  ok &= ExpectField(payload, "evidence_contract",
                    "compatibility_datatype_descriptor_evidence.v1", label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= Expect(Contains(payload, "\"datatype_reference_count\":"),
               std::string(label) + " missing datatype reference count");
  ok &= ExpectBool(payload, "datatype_surface_matched", true, label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "wire_literal_cast_comparison_required", true, label);
  ok &= ExpectBool(payload, "generic_text_fallback_allowed", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "compatibility_sql_executed", false, label);
  ok &= ExpectField(payload, "exactness_status",
                    "descriptor_surface_recorded_exactness_proof_pending", label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready", label);
  return ok;
}

std::string_view CompatibilityProfileUuid(std::string_view dialect) {
  if (dialect == "firebird") {
    return "019e13c0-0000-7000-8000-000000000302";
  }
  if (dialect == "mysql") {
    return "019e13c0-0000-7000-8000-000000000303";
  }
  if (dialect == "postgresql") {
    return "019e13c0-0000-7000-8000-000000000304";
  }
  return "";
}

bool ExpectDatatypeProfile(
    std::string_view payload,
    std::string_view dialect,
    std::initializer_list<std::string_view> expected_families,
    std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"datatype_profile_evidence\":{"),
               std::string(label) + " missing datatype profile evidence");
  ok &= ExpectField(payload, "evidence_contract",
                    "compatibility_datatype_profile_family_detection.v1", label);
  ok &= ExpectField(payload, "dialect", dialect, label);
  ok &= ExpectField(payload, "compatibility_profile_uuid", CompatibilityProfileUuid(dialect),
                    label);
  ok &= ExpectField(payload, "descriptor_authority",
                    "scratchbird_engine_catalog", label);
  ok &= Expect(Contains(payload, "\"detected_family_count\":"),
               std::string(label) + " missing detected family count");
  ok &= Expect(!Contains(payload, "\"detected_family_count\":0"),
               std::string(label) + " did not detect any datatype families");
  for (const auto family : expected_families) {
    ok &= ExpectBool(payload, family, true, label);
  }
  ok &= ExpectBool(payload, "source_text_included", false, label);
  ok &= ExpectBool(payload, "generic_text_fallback_allowed", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "compatibility_sql_executed", false, label);
  ok &= ExpectBool(payload, "exact_binary_wire_literal_cast_comparison_required",
                   true, label);
  ok &= ExpectField(payload, "runtime_equivalence_status",
                    "pending_compatibility_native_exactness_replay", label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready",
                    label);
  ok &= Expect(!Contains(payload, "mysql_lts"),
               std::string(label) + " emitted mysql_lts runtime evidence");
  return ok;
}

bool FamilyExpected(std::initializer_list<std::string_view> expected_families,
                    std::string_view family) {
  for (const auto expected : expected_families) {
    if (expected == family) return true;
  }
  return false;
}

bool ExpectFirebirdExactDatatypeDomainEvidence(
    std::string_view payload,
    std::string_view operation_family,
    std::initializer_list<std::string_view> expected_families,
    std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload,
                        "\"firebird_exact_datatype_domain_evidence\":{"),
               std::string(label) +
                   " missing Firebird exact datatype/domain evidence");
  ok &= ExpectField(
      payload, "evidence_contract",
      "firebird_exact_datatype_domain_descriptor_evidence.v1", label);
  ok &= ExpectField(payload, "dialect", "firebird", label);
  ok &= ExpectField(payload, "operation_family", operation_family, label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "descriptor_authority",
                    "scratchbird_engine_catalog", label);
  ok &= ExpectField(payload, "execution_authority",
                    "scratchbird_engine_sblr", label);
  ok &= ExpectBool(payload, "sblr_operation_uuid_resolution_required", true,
                   label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "generic_text_fallback_allowed", false, label);
  ok &= ExpectBool(payload, "source_text_included", false, label);
  ok &= ExpectBool(payload, "object_name_text_included", false, label);
  ok &= ExpectBool(payload, "literal_text_included", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false,
                   label);
  ok &= ExpectBool(payload, "compatibility_sql_executed", false, label);
  ok &= Expect(Contains(payload, "\"descriptor_family_count\":"),
               std::string(label) +
                   " missing Firebird descriptor family count");
  ok &= Expect(!Contains(payload, "\"descriptor_family_count\":0"),
               std::string(label) +
                   " recorded zero Firebird descriptor families");
  if (operation_family == "firebird.ddl.create") {
    ok &= ExpectBool(payload, "table_column_descriptor_bound", true, label);
  }
  if (FamilyExpected(expected_families, "range_domain_composite")) {
    ok &= ExpectBool(payload, "domain_descriptor_bound", true, label);
  }
  if (FamilyExpected(expected_families, "numeric")) {
    ok &= ExpectBool(payload, "exact_numeric_descriptor_bound", true, label);
  }
  if (FamilyExpected(expected_families, "exact_decimal")) {
    ok &= ExpectBool(payload, "numeric_precision_scale_descriptor_bound",
                     true, label);
  }
  if (FamilyExpected(expected_families, "floating")) {
    ok &= ExpectBool(payload, "floating_descriptor_bound", true, label);
  }
  if (FamilyExpected(expected_families, "text")) {
    ok &= ExpectBool(payload, "text_descriptor_bound", true, label);
  }
  if (FamilyExpected(expected_families, "charset_collation_sensitive_text")) {
    ok &= ExpectBool(payload, "charset_descriptor_bound", true, label);
    ok &= ExpectBool(payload, "collation_descriptor_bound", true, label);
  }
  if (FamilyExpected(expected_families, "binary_blob")) {
    ok &= ExpectBool(payload, "blob_descriptor_bound", true, label);
  }
  if (FamilyExpected(expected_families, "temporal")) {
    ok &= ExpectBool(payload, "temporal_descriptor_bound", true, label);
  }
  if (FamilyExpected(expected_families, "boolean")) {
    ok &= ExpectBool(payload, "boolean_descriptor_bound", true, label);
  }
  if (FamilyExpected(expected_families, "array")) {
    ok &= ExpectBool(payload, "array_bounds_descriptor_bound", true, label);
  }
  ok &= ExpectField(
      payload, "descriptor_exactness_status",
      "firebird_exact_datatype_descriptor_recorded_runtime_equivalence_pending",
      label);
  ok &= ExpectField(payload, "runtime_equivalence_status",
                    "pending_compatibility_native_exactness_replay", label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready",
                    label);
  return ok;
}

bool ExpectProceduralSourceRetention(std::string_view payload,
                                     std::string_view label) {
  const bool firebird_payload = Contains(payload, "\"dialect\":\"firebird\"");
  bool ok = true;
  ok &= Expect(Contains(payload, "\"procedural_body_source_retention_evidence\":{"),
               std::string(label) + " missing source-retention evidence");
  ok &= ExpectField(payload, "evidence_contract",
                    "compatibility_procedural_body_source_retention.v1", label);
  ok &= ExpectField(payload, "source_retention_state",
                    "catalog_reference_audit_material", label);
  ok &= ExpectBool(payload, "catalog_source_reference_required", true, label);
  ok &= ExpectBool(payload, "catalog_audit_material", true, label);
  ok &= ExpectBool(payload, "raw_sql_body_embedded_in_sblr_envelope", false, label);
  ok &= ExpectBool(payload, "body_text_redacted_from_parser_evidence", true, label);
  ok &= ExpectBool(payload, "uuid_binding_required", true, label);
  ok &= ExpectField(payload, "execution_authority",
                    "scratchbird_engine_sblr", label);
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
  ok &= ExpectField(
      payload, "executable_sblr_lowering_status",
      firebird_payload ? "parser_bound_sblr_instruction_stream_encoded"
                       : "pending",
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
    ok &= ExpectField(payload, "execution_authority",
                      "scratchbird_engine_sblr", label);
    ok &= ExpectField(payload, "runtime_equivalence_status",
                      "pending_compatibility_native_psql_replay", label);
    ok &= Expect(Contains(payload, "\"encoded_instruction_count\":"),
                 std::string(label) +
                     " missing Firebird encoded instruction count");
  }
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready", label);
  return ok;
}

template <typename Result>
bool ExpectDatatypeResult(const Result& result,
                          std::string_view dialect,
                          std::string_view operation_family,
                          std::initializer_list<std::string_view> expected_families,
                          std::initializer_list<std::string_view> source_markers,
                          std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " did not parse");
  ok &= ExpectEnvelopeAuthority(result.sblr_envelope, dialect, operation_family, label);
  ok &= ExpectDatatypeDescriptor(result.sblr_envelope, label);
  ok &= ExpectDatatypeProfile(result.sblr_envelope, dialect, expected_families,
                              label);
  if (dialect == "firebird") {
    ok &= ExpectFirebirdExactDatatypeDomainEvidence(
        result.sblr_envelope, operation_family, expected_families, label);
  }
  ok &= ExpectCommonParserEvidence(result.parser_evidence_json, dialect, label);
  ok &= ExpectDatatypeDescriptor(result.parser_evidence_json, label);
  ok &= ExpectDatatypeProfile(result.parser_evidence_json, dialect,
                              expected_families, label);
  if (dialect == "firebird") {
    ok &= ExpectFirebirdExactDatatypeDomainEvidence(
        result.parser_evidence_json, operation_family, expected_families,
        label);
  }
  ok &= ExpectNoMarkers(result.sblr_envelope, source_markers, label);
  ok &= ExpectNoMarkers(result.parser_evidence_json, source_markers, label);
  return ok;
}

template <typename Result>
bool ExpectProceduralResult(const Result& result,
                            std::string_view dialect,
                            std::string_view operation_family,
                            std::initializer_list<std::string_view> source_markers,
                            std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " did not parse");
  ok &= ExpectEnvelopeAuthority(result.sblr_envelope, dialect, operation_family, label);
  ok &= ExpectProceduralSourceRetention(result.sblr_envelope, label);
  ok &= ExpectCommonParserEvidence(result.parser_evidence_json, dialect, label);
  ok &= ExpectProceduralSourceRetention(result.parser_evidence_json, label);
  ok &= ExpectNoMarkers(result.sblr_envelope, source_markers, label);
  ok &= ExpectNoMarkers(result.parser_evidence_json, source_markers, label);
  return ok;
}

template <typename UdrResult>
bool ExpectUdrDatatypeResult(const UdrResult& result,
                             std::string_view dialect,
                             std::string_view operation_family,
                             std::initializer_list<std::string_view> expected_families,
                             std::initializer_list<std::string_view> source_markers,
                             std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelopeAuthority(result.payload, dialect, operation_family, label);
  ok &= ExpectDatatypeDescriptor(result.payload, label);
  ok &= ExpectDatatypeProfile(result.payload, dialect, expected_families,
                              label);
  if (dialect == "firebird") {
    ok &= ExpectFirebirdExactDatatypeDomainEvidence(
        result.payload, operation_family, expected_families, label);
  }
  ok &= ExpectNoMarkers(result.payload, source_markers, label);
  return ok;
}

template <typename UdrResult>
bool ExpectUdrProceduralResult(const UdrResult& result,
                               std::string_view dialect,
                               std::string_view operation_family,
                               std::initializer_list<std::string_view> source_markers,
                               std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelopeAuthority(result.payload, dialect, operation_family, label);
  ok &= ExpectProceduralSourceRetention(result.payload, label);
  ok &= ExpectNoMarkers(result.payload, source_markers, label);
  return ok;
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;
  const auto firebird_type_table = ParseStatement(
      "create table fb_proof_type_table (fb_proof_id integer, fb_proof_amount numeric(10,2), fb_proof_note varchar(40) character set utf8 collate unicode, fb_proof_blob blob sub_type text segment size 80, fb_proof_at timestamp with time zone, fb_proof_active boolean default true, fb_proof_slots integer[3])");
  ok &= ExpectDatatypeResult(
      firebird_type_table,
      "firebird", "firebird.ddl.create",
      {"numeric", "exact_decimal", "text",
       "charset_collation_sensitive_text", "binary_blob", "temporal",
       "boolean", "array"},
      {"fb_proof_type_table", "fb_proof_id", "fb_proof_amount",
       "fb_proof_note", "fb_proof_blob", "fb_proof_at", "fb_proof_active",
       "fb_proof_slots"},
      "firebird direct datatype");
  ok &= ExpectBool(firebird_type_table.sblr_envelope,
                   "blob_subtype_descriptor_bound", true,
                   "firebird direct datatype");
  ok &= ExpectBool(firebird_type_table.sblr_envelope,
                   "blob_segment_size_descriptor_bound", true,
                   "firebird direct datatype");
  ok &= ExpectBool(firebird_type_table.sblr_envelope,
                   "temporal_timezone_descriptor_bound", true,
                   "firebird direct datatype");
  ok &= ExpectBool(firebird_type_table.sblr_envelope,
                   "default_descriptor_bound", true,
                   "firebird direct datatype");
  const auto firebird_domain = ParseStatement(
      "create domain fb_proof_domain as varchar(32) character set utf8 collate unicode default 'fb_domain_secret' not null check (value <> '')");
  ok &= ExpectDatatypeResult(
      firebird_domain, "firebird", "firebird.datatype.domain.create",
      {"text", "charset_collation_sensitive_text", "range_domain_composite"},
      {"fb_proof_domain", "fb_domain_secret"},
      "firebird direct domain datatype");
  ok &= ExpectBool(firebird_domain.sblr_envelope,
                   "domain_descriptor_bound", true,
                   "firebird direct domain datatype");
  ok &= ExpectBool(firebird_domain.sblr_envelope,
                   "default_descriptor_bound", true,
                   "firebird direct domain datatype");
  ok &= ExpectBool(firebird_domain.sblr_envelope,
                   "nullability_descriptor_bound", true,
                   "firebird direct domain datatype");
  ok &= ExpectBool(firebird_domain.sblr_envelope,
                   "check_constraint_descriptor_bound", true,
                   "firebird direct domain datatype");
  ok &= ExpectProceduralResult(
      ParseStatement("create procedure fb_proof_proc as begin post_event 'fb_proof_secret_body'; end"),
      "firebird", "firebird.ddl.create.procedure",
      {"fb_proof_proc", "fb_proof_secret_body", "post_event"},
      "firebird direct procedure");
  ok &= ExpectProceduralResult(
      ParseStatement("recreate procedure fb_recreate_proc as begin suspend; end"),
      "firebird", "firebird.ddl.recreate.procedure",
      {"fb_recreate_proc", "suspend"},
      "firebird direct recreate procedure");
  ok &= ExpectProceduralResult(
      ParseStatement("create trigger fb_proof_bi for fb_proof_type_table before insert as begin new.fb_proof_id = gen_id(fb_proof_seq, 1); end"),
      "firebird", "firebird.ddl.create.trigger",
      {"fb_proof_bi", "fb_proof_type_table", "fb_proof_id",
       "fb_proof_seq"},
      "firebird direct trigger");
  ok &= ExpectProceduralResult(
      ParseStatement("create package body fb_proof_pkg as begin procedure apply_change as begin post_event 'fb_pkg_secret_body'; end end"),
      "firebird", "firebird.ddl.create.package_body",
      {"fb_proof_pkg", "apply_change", "fb_pkg_secret_body", "post_event"},
      "firebird direct package body");
  ok &= ExpectProceduralResult(
      ParseStatement("execute block as begin post_event 'fb_block_secret_body'; suspend; end"),
      "firebird", "firebird.psql.execute_block",
      {"fb_block_secret_body", "post_event", "suspend"},
      "firebird direct execute block");
  const auto firebird_udr_type_table = sbu_firebird_parse_to_sblr(
      "create table fb_udr_proof_type_table (fb_udr_proof_id integer, fb_udr_proof_amount numeric(10,2), fb_udr_proof_note varchar(40) character set utf8 collate unicode, fb_udr_proof_blob blob sub_type text segment size 80, fb_udr_proof_at timestamp with time zone, fb_udr_proof_active boolean default true, fb_udr_proof_slots integer[3])",
      trusted);
  ok &= ExpectUdrDatatypeResult(
      firebird_udr_type_table,
      "firebird", "firebird.ddl.create",
      {"numeric", "exact_decimal", "text",
       "charset_collation_sensitive_text", "binary_blob", "temporal",
       "boolean", "array"},
      {"fb_udr_proof_type_table", "fb_udr_proof_id",
       "fb_udr_proof_amount", "fb_udr_proof_note", "fb_udr_proof_blob",
       "fb_udr_proof_at", "fb_udr_proof_active", "fb_udr_proof_slots"},
      "firebird UDR datatype");
  ok &= ExpectBool(firebird_udr_type_table.payload,
                   "blob_subtype_descriptor_bound", true,
                   "firebird UDR datatype");
  ok &= ExpectBool(firebird_udr_type_table.payload,
                   "blob_segment_size_descriptor_bound", true,
                   "firebird UDR datatype");
  ok &= ExpectBool(firebird_udr_type_table.payload,
                   "temporal_timezone_descriptor_bound", true,
                   "firebird UDR datatype");
  const auto firebird_udr_domain = sbu_firebird_parse_to_sblr(
      "create domain fb_udr_proof_domain as varchar(32) character set utf8 collate unicode default 'fb_udr_domain_secret' not null check (value <> '')",
      trusted);
  ok &= ExpectUdrDatatypeResult(
      firebird_udr_domain, "firebird", "firebird.datatype.domain.create",
      {"text", "charset_collation_sensitive_text", "range_domain_composite"},
      {"fb_udr_proof_domain", "fb_udr_domain_secret"},
      "firebird UDR domain datatype");
  ok &= ExpectBool(firebird_udr_domain.payload, "domain_descriptor_bound",
                   true, "firebird UDR domain datatype");
  ok &= ExpectBool(firebird_udr_domain.payload, "default_descriptor_bound",
                   true, "firebird UDR domain datatype");
  ok &= ExpectBool(firebird_udr_domain.payload, "nullability_descriptor_bound",
                   true, "firebird UDR domain datatype");
  ok &= ExpectBool(firebird_udr_domain.payload,
                   "check_constraint_descriptor_bound", true,
                   "firebird UDR domain datatype");
  ok &= ExpectUdrProceduralResult(
      sbu_firebird_parse_to_sblr(
          "create procedure fb_udr_proof_proc as begin post_event 'fb_udr_proof_secret_body'; end",
          trusted),
      "firebird", "firebird.ddl.create.procedure",
      {"fb_udr_proof_proc", "fb_udr_proof_secret_body", "post_event"},
      "firebird UDR procedure");
  ok &= ExpectUdrProceduralResult(
      sbu_firebird_parse_to_sblr(
          "execute block as begin post_event 'fb_udr_block_secret_body'; suspend; end",
          trusted),
      "firebird", "firebird.psql.execute_block",
      {"fb_udr_block_secret_body", "post_event", "suspend"},
      "firebird UDR execute block");
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;
  ok &= ExpectDatatypeResult(
      ParseStatement("create table mysql_proof_type_table (mysql_proof_id int, mysql_proof_amount decimal(10,2), mysql_proof_score double, mysql_proof_note varchar(40) character set utf8mb4 collate utf8mb4_bin, mysql_proof_blob blob, mysql_proof_hash binary(16), mysql_proof_at timestamp, mysql_proof_year year, mysql_proof_active boolean, mysql_proof_doc json, mysql_proof_kind enum('a','b'), mysql_proof_flags set('x','y'), mysql_proof_place point)"),
      "mysql", "mysql.ddl.create",
      {"numeric", "exact_decimal", "floating", "text",
       "charset_collation_sensitive_text", "binary_blob", "temporal",
       "boolean", "json_document", "enum_set", "geometric_spatial"},
      {"mysql_proof_type_table", "mysql_proof_id", "mysql_proof_amount",
       "mysql_proof_score", "mysql_proof_note", "mysql_proof_blob",
       "mysql_proof_hash", "mysql_proof_at", "mysql_proof_year",
       "mysql_proof_active", "mysql_proof_doc", "mysql_proof_kind",
       "mysql_proof_flags", "mysql_proof_place"},
      "mysql direct datatype");
  ok &= ExpectProceduralResult(
      ParseStatement("create procedure mysql_proof_proc() begin select 'mysql_proof_secret_body'; end"),
      "mysql", "mysql.routine.procedure.create",
      {"mysql_proof_proc", "mysql_proof_secret_body"},
      "mysql direct procedure");
  ok &= ExpectUdrDatatypeResult(
      sbu_mysql_parse_to_sblr(
          "create table mysql_udr_proof_type_table (mysql_udr_proof_id int, mysql_udr_proof_amount decimal(10,2), mysql_udr_proof_score double, mysql_udr_proof_note varchar(40) character set utf8mb4 collate utf8mb4_bin, mysql_udr_proof_blob blob, mysql_udr_proof_hash binary(16), mysql_udr_proof_at timestamp, mysql_udr_proof_year year, mysql_udr_proof_active boolean, mysql_udr_proof_doc json, mysql_udr_proof_kind enum('a','b'), mysql_udr_proof_flags set('x','y'), mysql_udr_proof_place point)",
          trusted),
      "mysql", "mysql.ddl.create",
      {"numeric", "exact_decimal", "floating", "text",
       "charset_collation_sensitive_text", "binary_blob", "temporal",
       "boolean", "json_document", "enum_set", "geometric_spatial"},
      {"mysql_udr_proof_type_table", "mysql_udr_proof_id",
       "mysql_udr_proof_amount", "mysql_udr_proof_score",
       "mysql_udr_proof_note", "mysql_udr_proof_blob",
       "mysql_udr_proof_hash", "mysql_udr_proof_at",
       "mysql_udr_proof_year", "mysql_udr_proof_active",
       "mysql_udr_proof_doc", "mysql_udr_proof_kind",
       "mysql_udr_proof_flags", "mysql_udr_proof_place"},
      "mysql UDR datatype");
  ok &= ExpectUdrProceduralResult(
      sbu_mysql_parse_to_sblr(
          "create procedure mysql_udr_proof_proc() begin select 'mysql_udr_proof_secret_body'; end",
          trusted),
      "mysql", "mysql.routine.procedure.create",
      {"mysql_udr_proof_proc", "mysql_udr_proof_secret_body"},
      "mysql UDR procedure");
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::sbu_postgresql_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;
  ok &= ExpectDatatypeResult(
      ParseStatement("create table pg_proof_type_table (pg_proof_id integer, pg_proof_amount numeric(10,2), pg_proof_score double precision, pg_proof_note varchar(40), pg_proof_at timestamptz, pg_proof_doc jsonb, pg_proof_uuid uuid, pg_proof_addr inet, pg_proof_tags integer[], pg_proof_window int4range)"),
      "postgresql", "postgresql.ddl.create",
      {"numeric", "exact_decimal", "floating", "text", "temporal",
       "json_document", "uuid", "array", "network",
       "range_domain_composite"},
      {"pg_proof_type_table", "pg_proof_id", "pg_proof_amount",
       "pg_proof_score", "pg_proof_note", "pg_proof_at", "pg_proof_doc",
       "pg_proof_uuid", "pg_proof_addr", "pg_proof_tags",
       "pg_proof_window"},
      "postgresql direct datatype");
  ok &= ExpectProceduralResult(
      ParseStatement("create procedure pg_proof_proc() language sql as 'select 1 /* pg_proof_secret_body */'"),
      "postgresql", "postgresql.routine.procedure.create",
      {"pg_proof_proc", "pg_proof_secret_body"},
      "postgresql direct procedure");
  ok &= ExpectUdrDatatypeResult(
      sbu_postgresql_parse_to_sblr(
          "create table pg_udr_proof_type_table (pg_udr_proof_id integer, pg_udr_proof_amount numeric(10,2), pg_udr_proof_score double precision, pg_udr_proof_note varchar(40), pg_udr_proof_at timestamptz, pg_udr_proof_doc jsonb, pg_udr_proof_uuid uuid, pg_udr_proof_addr inet, pg_udr_proof_tags integer[], pg_udr_proof_window int4range)",
          trusted),
      "postgresql", "postgresql.ddl.create",
      {"numeric", "exact_decimal", "floating", "text", "temporal",
       "json_document", "uuid", "array", "network",
       "range_domain_composite"},
      {"pg_udr_proof_type_table", "pg_udr_proof_id", "pg_udr_proof_amount",
       "pg_udr_proof_score", "pg_udr_proof_note", "pg_udr_proof_at",
       "pg_udr_proof_doc", "pg_udr_proof_uuid", "pg_udr_proof_addr",
       "pg_udr_proof_tags", "pg_udr_proof_window"},
      "postgresql UDR datatype");
  ok &= ExpectUdrProceduralResult(
      sbu_postgresql_parse_to_sblr(
          "create procedure pg_udr_proof_proc() language sql as 'select 1 /* pg_udr_proof_secret_body */'",
          trusted),
      "postgresql", "postgresql.routine.procedure.create",
      {"pg_udr_proof_proc", "pg_udr_proof_secret_body"},
      "postgresql UDR procedure");
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
