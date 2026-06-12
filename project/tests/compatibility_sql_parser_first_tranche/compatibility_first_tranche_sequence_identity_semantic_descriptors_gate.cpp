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

struct SequenceExpected {
  std::string_view dialect;
  std::string_view release_profile;
  std::string_view compatibility_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view sequence_identity_profile;
  std::string_view sequence_identity_surface;
  bool create_sequence_or_generator_surface{false};
  bool alter_sequence_surface{false};
  bool auto_increment_surface{false};
  bool last_insert_id_surface{false};
  bool next_value_surface{false};
  bool currval_surface{false};
  bool setval_surface{false};
  bool sequence_backed_default_present{false};
  bool restart_descriptor_present{false};
  bool increment_descriptor_present{false};
  bool min_value_descriptor_present{false};
  bool max_value_descriptor_present{false};
  bool cycle_descriptor_present{false};
  bool cache_descriptor_present{false};
  bool session_visible_state_surface{false};
  std::string_view object_identity_policy;
  std::string_view engine_catalog_sequence_descriptor_policy;
  std::string_view allocation_finality_policy;
  std::string_view lower_layer_allocation_policy;
  std::string_view value_function_profile;
  std::string_view session_visibility_policy;
  std::string_view sequence_backed_default_policy;
  std::string_view restart_increment_descriptor_policy;
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

bool ExpectSequenceEvidence(std::string_view payload,
                            const SequenceExpected& expected,
                            std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"sequence_identity_semantic_evidence\":{"),
               std::string(label) + " missing sequence identity evidence");
  ok &= ExpectField(payload, "evidence_contract",
                    "compatibility_sequence_identity_semantic_descriptor_evidence.v1",
                    label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "compatibility_profile_uuid",
                    expected.compatibility_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid",
                    expected.semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "release_profile", expected.release_profile, label);
  ok &= ExpectField(payload, "sequence_identity_profile",
                    expected.sequence_identity_profile, label);
  ok &= ExpectField(payload, "sequence_identity_surface",
                    expected.sequence_identity_surface, label);
  ok &= ExpectBool(payload, "create_sequence_or_generator_surface",
                   expected.create_sequence_or_generator_surface, label);
  ok &= ExpectBool(payload, "alter_sequence_surface",
                   expected.alter_sequence_surface, label);
  ok &= ExpectBool(payload, "auto_increment_surface",
                   expected.auto_increment_surface, label);
  ok &= ExpectBool(payload, "last_insert_id_surface",
                   expected.last_insert_id_surface, label);
  ok &= ExpectBool(payload, "next_value_surface",
                   expected.next_value_surface, label);
  ok &= ExpectBool(payload, "currval_surface", expected.currval_surface, label);
  ok &= ExpectBool(payload, "setval_surface", expected.setval_surface, label);
  ok &= ExpectBool(payload, "sequence_backed_default_present",
                   expected.sequence_backed_default_present, label);
  ok &= ExpectBool(payload, "restart_descriptor_present",
                   expected.restart_descriptor_present, label);
  ok &= ExpectBool(payload, "increment_descriptor_present",
                   expected.increment_descriptor_present, label);
  ok &= ExpectBool(payload, "min_value_descriptor_present",
                   expected.min_value_descriptor_present, label);
  ok &= ExpectBool(payload, "max_value_descriptor_present",
                   expected.max_value_descriptor_present, label);
  ok &= ExpectBool(payload, "cycle_descriptor_present",
                   expected.cycle_descriptor_present, label);
  ok &= ExpectBool(payload, "cache_descriptor_present",
                   expected.cache_descriptor_present, label);
  ok &= ExpectBool(payload, "session_visible_state_surface",
                   expected.session_visible_state_surface, label);
  ok &= ExpectField(payload, "object_identity_policy",
                    expected.object_identity_policy, label);
  ok &= ExpectBool(payload, "uuid_required_object_identity", true, label);
  ok &= ExpectField(payload, "engine_catalog_sequence_descriptor_policy",
                    expected.engine_catalog_sequence_descriptor_policy, label);
  ok &= ExpectField(payload, "allocation_finality_policy",
                    expected.allocation_finality_policy, label);
  ok &= ExpectField(payload, "lower_layer_allocation_policy",
                    expected.lower_layer_allocation_policy, label);
  ok &= ExpectField(payload, "value_function_profile",
                    expected.value_function_profile, label);
  ok &= ExpectField(payload, "session_visibility_policy",
                    expected.session_visibility_policy, label);
  ok &= ExpectField(payload, "sequence_backed_default_policy",
                    expected.sequence_backed_default_policy, label);
  ok &= ExpectField(payload, "restart_increment_descriptor_policy",
                    expected.restart_increment_descriptor_policy, label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "source_sql_text_included", false, label);
  ok &= ExpectBool(payload, "original_sql_identifier_text_included", false, label);
  ok &= ExpectBool(payload, "object_name_text_included", false, label);
  ok &= ExpectBool(payload, "sblr_embeds_source_identifiers", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false, label);
  ok &= ExpectBool(payload, "parser_sequence_value_authority", false, label);
  ok &= ExpectBool(payload, "compatibility_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(payload, "descriptor_exactness_status",
                    "parser_sequence_identity_descriptor_recorded_runtime_equivalence_pending",
                    label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready", label);
  return ok;
}

template <typename Result>
bool ExpectDirectSequenceResult(const Result& result,
                                const SequenceExpected& expected,
                                std::string_view operation_family,
                                std::initializer_list<std::string_view> markers,
                                std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " did not parse: " +
                             result.message_vector_json);
  ok &= Expect(result.operation_family == operation_family,
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
  ok &= ExpectBool(result.parser_evidence_json,
                   "parser_sequence_value_authority", false, label);
  ok &= ExpectSequenceEvidence(result.parser_evidence_json, expected, label);
  ok &= ExpectEnvelopeAuthority(result.sblr_envelope, expected.dialect,
                                operation_family, label);
  ok &= ExpectSequenceEvidence(result.sblr_envelope, expected, label);
  ok &= ExpectNoMarkers(result.parser_evidence_json, markers, label);
  ok &= ExpectNoMarkers(result.sblr_envelope, markers, label);
  ok &= ExpectNoMysqlLts(result.parser_evidence_json, label);
  ok &= ExpectNoMysqlLts(result.sblr_envelope, label);
  return ok;
}

template <typename UdrResult>
bool ExpectUdrSequenceResult(const UdrResult& result,
                             const SequenceExpected& expected,
                             std::string_view operation_family,
                             std::initializer_list<std::string_view> markers,
                             std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelopeAuthority(result.payload, expected.dialect,
                                operation_family, label);
  ok &= ExpectSequenceEvidence(result.payload, expected, label);
  ok &= ExpectNoMarkers(result.payload, markers, label);
  ok &= ExpectNoMysqlLts(result.payload, label);
  return ok;
}

SequenceExpected FirebirdCreateSequenceExpected() {
  return {"firebird",
          "5.0.4",
          "019e13c0-0000-7000-8000-000000000302",
          "019e13c0-1300-7000-8000-000000000302",
          "firebird.sequence_generator_identity_profile",
          "firebird_create_sequence",
          true,
          false,
          false,
          false,
          false,
          false,
          false,
          false,
          true,
          true,
          false,
          false,
          false,
          false,
          false,
          "firebird_sequence_generator_uuid_required_no_source_name_binding",
          "firebird_engine_catalog_generator_sequence_descriptor_policy",
          "firebird_generator_nontransactional_allocation_descriptor_parser_not_allocator",
          "firebird_engine_sequence_catalog_allocates_values_outside_parser",
          "firebird_generator_function_not_observed",
          "firebird_generator_values_visible_immediately_no_parser_session_state",
          "firebird_no_identity_default_observed",
          "firebird_restart_with_and_increment_by_descriptor"};
}

SequenceExpected FirebirdCreateGeneratorExpected() {
  auto expected = FirebirdCreateSequenceExpected();
  expected.sequence_identity_surface = "firebird_create_generator";
  expected.restart_descriptor_present = false;
  expected.increment_descriptor_present = false;
  return expected;
}

SequenceExpected FirebirdValueExpected() {
  auto expected = FirebirdCreateSequenceExpected();
  expected.sequence_identity_surface = "firebird_generator_value_expression";
  expected.create_sequence_or_generator_surface = false;
  expected.next_value_surface = true;
  expected.restart_descriptor_present = false;
  expected.increment_descriptor_present = false;
  expected.value_function_profile =
      "firebird_gen_id_and_next_value_for_descriptor";
  return expected;
}

SequenceExpected MysqlAutoIncrementExpected() {
  return {"mysql",
          "9.7.0",
          "019e13c0-0000-7000-8000-000000000303",
          "019e13c0-1300-7000-8000-000000000303",
          "mysql.auto_increment_identity_profile",
          "mysql_auto_increment_column_or_table_option",
          false,
          false,
          true,
          false,
          false,
          false,
          false,
          true,
          false,
          true,
          false,
          false,
          false,
          false,
          false,
          "mysql_table_column_auto_increment_uuid_required_no_source_name_binding",
          "mysql_engine_catalog_auto_increment_counter_descriptor_policy",
          "mysql_auto_increment_lower_layer_allocation_descriptor_parser_not_allocator",
          "mysql_storage_engine_auto_increment_allocator_policy_descriptor",
          "mysql_last_insert_id_not_observed",
          "mysql_last_insert_id_connection_session_visible_descriptor",
          "mysql_auto_increment_column_counter_default_descriptor",
          "mysql_auto_increment_table_option_descriptor"};
}

SequenceExpected MysqlLastInsertIdExpected() {
  auto expected = MysqlAutoIncrementExpected();
  expected.sequence_identity_surface = "mysql_last_insert_id_session_function";
  expected.auto_increment_surface = false;
  expected.last_insert_id_surface = true;
  expected.sequence_backed_default_present = false;
  expected.increment_descriptor_present = false;
  expected.session_visible_state_surface = true;
  expected.value_function_profile =
      "mysql_last_insert_id_session_visible_descriptor";
  expected.sequence_backed_default_policy =
      "mysql_no_auto_increment_default_observed";
  return expected;
}

SequenceExpected PostgresqlCreateSequenceExpected() {
  return {"postgresql",
          "18.3",
          "019e13c0-0000-7000-8000-000000000304",
          "019e13c0-1300-7000-8000-000000000304",
          "postgresql.sequence_serial_identity_profile",
          "postgresql_create_sequence",
          true,
          false,
          false,
          false,
          false,
          false,
          false,
          false,
          true,
          true,
          true,
          true,
          true,
          true,
          false,
          "postgresql_sequence_and_owned_default_uuid_required_no_source_name_binding",
          "postgresql_engine_catalog_sequence_descriptor_policy",
          "postgresql_sequence_allocation_descriptor_parser_not_allocator",
          "postgresql_sequence_access_method_and_catalog_allocator_policy",
          "postgresql_sequence_function_not_observed",
          "postgresql_currval_session_requires_prior_nextval_descriptor",
          "postgresql_no_sequence_default_observed",
          "postgresql_start_restart_increment_min_max_cache_cycle_descriptor"};
}

SequenceExpected PostgresqlValueExpected() {
  auto expected = PostgresqlCreateSequenceExpected();
  expected.sequence_identity_surface = "postgresql_sequence_function_expression";
  expected.create_sequence_or_generator_surface = false;
  expected.next_value_surface = true;
  expected.currval_surface = true;
  expected.setval_surface = true;
  expected.restart_descriptor_present = false;
  expected.increment_descriptor_present = false;
  expected.min_value_descriptor_present = false;
  expected.max_value_descriptor_present = false;
  expected.cycle_descriptor_present = false;
  expected.cache_descriptor_present = false;
  expected.session_visible_state_surface = true;
  expected.value_function_profile =
      "postgresql_nextval_currval_setval_descriptor";
  return expected;
}

SequenceExpected PostgresqlSerialIdentityExpected() {
  auto expected = PostgresqlCreateSequenceExpected();
  expected.sequence_identity_surface =
      "postgresql_serial_and_identity_sequence_defaults";
  expected.create_sequence_or_generator_surface = false;
  expected.sequence_backed_default_present = true;
  expected.restart_descriptor_present = false;
  expected.increment_descriptor_present = false;
  expected.min_value_descriptor_present = false;
  expected.max_value_descriptor_present = false;
  expected.cycle_descriptor_present = false;
  expected.cache_descriptor_present = false;
  expected.sequence_backed_default_policy =
      "postgresql_serial_and_identity_sequence_backed_defaults";
  return expected;
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;

  constexpr std::string_view create_sequence =
      "create sequence FbSeqLeak start with 42 increment by 7";
  ok &= ExpectDirectSequenceResult(
      ParseStatement(create_sequence), FirebirdCreateSequenceExpected(),
      "firebird.ddl.create.sequence",
      {"FbSeqLeak", "FBSEQLEAK"}, "firebird direct create sequence");
  ok &= ExpectUdrSequenceResult(
      sbu_firebird_parse_to_sblr(create_sequence, trusted),
      FirebirdCreateSequenceExpected(), "firebird.ddl.create.sequence",
      {"FbSeqLeak", "FBSEQLEAK"}, "firebird UDR create sequence");

  constexpr std::string_view create_generator =
      "create generator \"ExactFbGeneratorLeak\"";
  ok &= ExpectDirectSequenceResult(
      ParseStatement(create_generator), FirebirdCreateGeneratorExpected(),
      "firebird.ddl.create.sequence",
      {"ExactFbGeneratorLeak", "EXACTFBGENERATORLEAK"},
      "firebird direct create generator");

  constexpr std::string_view value_query =
      "select gen_id(FbGenLeak, 5), next value for \"ExactFbNextLeak\" "
      "from rdb$database";
  ok &= ExpectDirectSequenceResult(
      ParseStatement(value_query), FirebirdValueExpected(),
      "firebird.expression.generator.gen_id",
      {"FbGenLeak", "FBGENLEAK", "ExactFbNextLeak", "EXACTFBNEXTLEAK"},
      "firebird direct gen_id next value");
  ok &= ExpectUdrSequenceResult(
      sbu_firebird_parse_to_sblr(value_query, trusted),
      FirebirdValueExpected(), "firebird.expression.generator.gen_id",
      {"FbGenLeak", "FBGENLEAK", "ExactFbNextLeak", "EXACTFBNEXTLEAK"},
      "firebird UDR gen_id next value");

  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;

  constexpr std::string_view create_table =
      "create table LeakDb.`ExactMysqlAiLeakTable` ("
      "LeakAiColumn bigint auto_increment primary key,"
      "LeakPayload varchar(20)) auto_increment = 9001";
  ok &= ExpectDirectSequenceResult(
      ParseStatement(create_table), MysqlAutoIncrementExpected(),
      "mysql.ddl.create",
      {"LeakDb", "LEAKDB", "ExactMysqlAiLeakTable",
       "EXACTMYSQLAILEAKTABLE", "LeakAiColumn", "LEAKAICOLUMN",
       "LeakPayload", "LEAKPAYLOAD"},
      "mysql direct auto increment");
  ok &= ExpectUdrSequenceResult(
      sbu_mysql_parse_to_sblr(create_table, trusted),
      MysqlAutoIncrementExpected(), "mysql.ddl.create",
      {"LeakDb", "LEAKDB", "ExactMysqlAiLeakTable",
       "EXACTMYSQLAILEAKTABLE", "LeakAiColumn", "LEAKAICOLUMN",
       "LeakPayload", "LEAKPAYLOAD"},
      "mysql UDR auto increment");

  constexpr std::string_view last_insert_id_query =
      "select last_insert_id() as LeakMysqlSessionValue";
  ok &= ExpectDirectSequenceResult(
      ParseStatement(last_insert_id_query), MysqlLastInsertIdExpected(),
      "mysql.query.select",
      {"LeakMysqlSessionValue", "LEAKMYSQLSESSIONVALUE"},
      "mysql direct last_insert_id");
  ok &= ExpectUdrSequenceResult(
      sbu_mysql_parse_to_sblr(last_insert_id_query, trusted),
      MysqlLastInsertIdExpected(), "mysql.query.select",
      {"LeakMysqlSessionValue", "LEAKMYSQLSESSIONVALUE"},
      "mysql UDR last_insert_id");

  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::sbu_postgresql_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  bool ok = true;

  constexpr std::string_view create_sequence =
      "create sequence \"ExactPgSeqLeak\" increment by 5 minvalue 10 "
      "maxvalue 999 start with 20 cache 3 cycle";
  ok &= ExpectDirectSequenceResult(
      ParseStatement(create_sequence), PostgresqlCreateSequenceExpected(),
      "postgresql.ddl.create",
      {"ExactPgSeqLeak", "EXACTPGSEQLEAK"},
      "postgresql direct create sequence");
  ok &= ExpectUdrSequenceResult(
      sbu_postgresql_parse_to_sblr(create_sequence, trusted),
      PostgresqlCreateSequenceExpected(), "postgresql.ddl.create",
      {"ExactPgSeqLeak", "EXACTPGSEQLEAK"},
      "postgresql UDR create sequence");

  constexpr std::string_view value_query =
      "select nextval('PgSeqLeak'), currval('PgSeqLeak'), "
      "setval('PgSeqLeak', 42, true)";
  ok &= ExpectDirectSequenceResult(
      ParseStatement(value_query), PostgresqlValueExpected(),
      "postgresql.query.select",
      {"PgSeqLeak", "PGSEQLEAK"}, "postgresql direct sequence functions");
  ok &= ExpectUdrSequenceResult(
      sbu_postgresql_parse_to_sblr(value_query, trusted),
      PostgresqlValueExpected(), "postgresql.query.select",
      {"PgSeqLeak", "PGSEQLEAK"}, "postgresql UDR sequence functions");

  constexpr std::string_view create_table =
      "create table PgSerialIdentityLeak ("
      "PgSerialLeak serial,"
      "PgIdentityLeak integer generated always as identity,"
      "PgPayloadLeak text)";
  ok &= ExpectDirectSequenceResult(
      ParseStatement(create_table), PostgresqlSerialIdentityExpected(),
      "postgresql.ddl.create",
      {"PgSerialIdentityLeak", "PGSERIALIDENTITYLEAK", "PgSerialLeak",
       "PGSERIALLEAK", "PgIdentityLeak", "PGIDENTITYLEAK", "PgPayloadLeak",
       "PGPAYLOADLEAK"},
      "postgresql direct serial identity");
  ok &= ExpectUdrSequenceResult(
      sbu_postgresql_parse_to_sblr(create_table, trusted),
      PostgresqlSerialIdentityExpected(), "postgresql.ddl.create",
      {"PgSerialIdentityLeak", "PGSERIALIDENTITYLEAK", "PgSerialLeak",
       "PGSERIALLEAK", "PgIdentityLeak", "PGIDENTITYLEAK", "PgPayloadLeak",
       "PGPAYLOADLEAK"},
      "postgresql UDR serial identity");

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
