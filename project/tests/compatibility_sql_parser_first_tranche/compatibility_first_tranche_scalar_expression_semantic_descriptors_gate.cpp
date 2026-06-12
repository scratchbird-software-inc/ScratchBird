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

struct ScalarExpected {
  std::string_view dialect;
  std::string_view release_profile;
  std::string_view compatibility_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view scalar_expression_profile;
  std::string_view operation_family;
  std::string_view cast_type_coercion_profile;
  std::string_view null_three_valued_logic_profile;
  std::string_view boolean_literal_profile;
  std::string_view string_comparison_collation_profile;
  std::string_view temporal_profile;
  std::string_view numeric_profile;
  std::string_view pattern_matching_profile;
  std::string_view conditional_expression_profile;
  std::string_view expression_builtin_profile;
  bool null_safe_equality_surface{false};
  bool is_distinct_from_surface{false};
  bool regexp_surface{false};
  bool similar_to_surface{false};
  bool compatibility_conditional_function_surface{false};
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
                 std::string(label) + " leaked scalar source marker: " +
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
                             const ScalarExpected& expected,
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

bool ExpectScalarEvidence(std::string_view payload,
                          const ScalarExpected& expected,
                          std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"scalar_expression_semantic_evidence\":{"),
               std::string(label) + " missing scalar expression evidence");
  ok &= ExpectField(payload, "evidence_contract",
                    "compatibility_scalar_expression_semantic_descriptor_evidence.v1",
                    label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "compatibility_profile_uuid",
                    expected.compatibility_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid",
                    expected.semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "release_profile", expected.release_profile, label);
  ok &= ExpectField(payload, "scalar_expression_profile",
                    expected.scalar_expression_profile, label);
  ok &= ExpectField(payload, "query_expression_surface",
                    "select_scalar_expression", label);
  ok &= ExpectField(payload, "cast_type_coercion_profile",
                    expected.cast_type_coercion_profile, label);
  ok &= ExpectField(payload, "null_three_valued_logic_profile",
                    expected.null_three_valued_logic_profile, label);
  ok &= ExpectField(payload, "boolean_literal_profile",
                    expected.boolean_literal_profile, label);
  ok &= ExpectField(payload, "string_comparison_collation_profile",
                    expected.string_comparison_collation_profile, label);
  ok &= ExpectField(
      payload, "temporal_literal_current_timestamp_date_arithmetic_profile",
      expected.temporal_profile, label);
  ok &= ExpectField(payload, "numeric_division_rounding_overflow_profile",
                    expected.numeric_profile, label);
  ok &= ExpectField(payload, "pattern_matching_profile",
                    expected.pattern_matching_profile, label);
  ok &= ExpectField(payload, "conditional_expression_profile",
                    expected.conditional_expression_profile, label);
  ok &= ExpectField(payload, "expression_builtin_profile",
                    expected.expression_builtin_profile, label);
  ok &= ExpectBool(payload, "cast_or_coercion_surface", true, label);
  ok &= ExpectBool(payload, "null_logic_surface", true, label);
  ok &= ExpectBool(payload, "boolean_literal_surface", true, label);
  ok &= ExpectBool(payload, "string_comparison_surface", true, label);
  ok &= ExpectBool(payload, "temporal_expression_surface", true, label);
  ok &= ExpectBool(payload, "numeric_expression_surface", true, label);
  ok &= ExpectBool(payload, "pattern_matching_surface", true, label);
  ok &= ExpectBool(payload, "conditional_expression_surface", true, label);
  ok &= ExpectBool(payload, "null_safe_equality_surface",
                   expected.null_safe_equality_surface, label);
  ok &= ExpectBool(payload, "is_distinct_from_surface",
                   expected.is_distinct_from_surface, label);
  ok &= ExpectBool(payload, "regexp_surface", expected.regexp_surface, label);
  ok &= ExpectBool(payload, "similar_to_surface",
                   expected.similar_to_surface, label);
  ok &= ExpectBool(payload, "compatibility_conditional_function_surface",
                   expected.compatibility_conditional_function_surface, label);
  ok &= ExpectBool(payload, "uuid_required_semantic_profile", true, label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectBool(payload, "source_sql_text_included", false, label);
  ok &= ExpectBool(payload, "literal_text_included", false, label);
  ok &= ExpectBool(payload, "object_name_text_included", false, label);
  ok &= ExpectBool(payload, "quoted_identifier_text_included", false, label);
  ok &= ExpectBool(payload, "sblr_embeds_source_identifiers", false, label);
  ok &= ExpectBool(payload, "parser_scalar_truth_authority", false, label);
  ok &= ExpectBool(payload, "parser_collation_authority", false, label);
  ok &= ExpectBool(payload, "parser_datatype_finality_authority", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false, label);
  ok &= ExpectBool(payload, "compatibility_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(
      payload, "descriptor_exactness_status",
      "parser_scalar_expression_descriptor_recorded_runtime_equivalence_pending",
      label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready", label);
  return ok;
}

template <typename Result>
bool ExpectDirectScalarResult(const Result& result,
                              const ScalarExpected& expected,
                              std::initializer_list<std::string_view> markers,
                              std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " did not parse: " +
                             result.message_vector_json);
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
  ok &= ExpectBool(result.parser_evidence_json,
                   "parser_sequence_value_authority", false, label);
  ok &= ExpectScalarEvidence(result.parser_evidence_json, expected, label);
  ok &= ExpectEnvelopeAuthority(result.sblr_envelope, expected, label);
  ok &= ExpectScalarEvidence(result.sblr_envelope, expected, label);
  ok &= ExpectNoMarkers(result.parser_evidence_json, markers, label);
  ok &= ExpectNoMarkers(result.sblr_envelope, markers, label);
  ok &= ExpectNoMysqlLts(result.parser_evidence_json, label);
  ok &= ExpectNoMysqlLts(result.sblr_envelope, label);
  return ok;
}

template <typename UdrResult>
bool ExpectUdrScalarResult(const UdrResult& result,
                           const ScalarExpected& expected,
                           std::initializer_list<std::string_view> markers,
                           std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelopeAuthority(result.payload, expected, label);
  ok &= ExpectScalarEvidence(result.payload, expected, label);
  ok &= ExpectNoMarkers(result.payload, markers, label);
  ok &= ExpectNoMysqlLts(result.payload, label);
  return ok;
}

ScalarExpected FirebirdExpected() {
  return {"firebird",
          "5.0.4",
          "019e13c0-0000-7000-8000-000000000302",
          "019e13c0-1400-7000-8000-000000000302",
          "firebird.scalar_expression_semantics_profile",
          "firebird.datatype.cast",
          "firebird_cast_domain_charset_decfloat_int128_descriptor",
          "firebird_boolean_unknown_three_valued_logic_profile",
          "firebird_boolean_true_false_unknown_literal_profile",
          "firebird_charset_collation_descriptor_no_parser_collation_authority",
          "firebird_date_time_timestamp_dateadd_datediff_descriptor",
          "firebird_exact_numeric_decfloat_int128_division_rounding_overflow_descriptor",
          "firebird_like_similar_to_containing_starting_with_descriptor",
          "firebird_coalesce_case_iif_nullif_decode_descriptor",
          "firebird_expression_builtin_profile_iif_dateadd_decfloat_int128",
          false,
          false,
          false,
          true,
          true};
}

ScalarExpected MysqlExpected() {
  return {"mysql",
          "9.7.0",
          "019e13c0-0000-7000-8000-000000000303",
          "019e13c0-1400-7000-8000-000000000303",
          "mysql.scalar_expression_semantics_profile",
          "mysql.query.select",
          "mysql_cast_convert_type_coercion_sql_mode_descriptor",
          "mysql_three_valued_logic_null_safe_equality_descriptor",
          "mysql_truthy_numeric_boolean_literal_profile",
          "mysql_charset_collation_coercibility_descriptor_no_parser_collation_authority",
          "mysql_datetime_timestamp_current_timestamp_date_add_sql_mode_descriptor",
          "mysql_division_rounding_overflow_sql_mode_descriptor",
          "mysql_like_regexp_rlike_collation_descriptor",
          "mysql_case_if_ifnull_nullif_coalesce_descriptor",
          "mysql_expression_builtin_profile_if_ifnull_date_add_regexp",
          true,
          false,
          true,
          false,
          true};
}

ScalarExpected PostgresqlExpected() {
  return {"postgresql",
          "18.3",
          "019e13c0-0000-7000-8000-000000000304",
          "019e13c0-1400-7000-8000-000000000304",
          "postgresql.scalar_expression_semantics_profile",
          "postgresql.query.select",
          "postgresql_cast_operator_type_resolution_descriptor",
          "postgresql_three_valued_logic_is_distinct_from_descriptor",
          "postgresql_strict_boolean_type_literal_profile",
          "postgresql_collation_operator_resolution_descriptor_no_parser_collation_authority",
          "postgresql_timestamp_timestamptz_interval_timezone_descriptor",
          "postgresql_numeric_division_rounding_overflow_descriptor",
          "postgresql_like_similar_to_regex_operator_descriptor",
          "postgresql_case_coalesce_nullif_descriptor",
          "postgresql_expression_operator_resolution_profile_is_distinct_from_similar_regex",
          false,
          true,
          true,
          true,
          false};
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view sql =
      "select cast(null as decfloat(16)) as fb_scalar_cast_alias,"
      "iif(unknown, current_timestamp, dateadd(day, 1, current_date)) "
      "as fb_scalar_temporal_alias,"
      "coalesce(nullif('fb_scalar_literal_alpha', 'fb_scalar_literal_beta'), "
      "'fb_scalar_literal_gamma') as fb_scalar_cond_alias,"
      "round(10 / 4) as fb_scalar_numeric_alias "
      "where 'fb_scalar_pattern_alpha' like 'fb_scalar_pattern_%' "
      "or 'fb_scalar_pattern_beta' similar to 'fb_scalar_pattern_%'";
  const std::initializer_list<std::string_view> markers = {
      "fb_scalar_cast_alias", "fb_scalar_temporal_alias",
      "fb_scalar_cond_alias", "fb_scalar_numeric_alias",
      "fb_scalar_literal_alpha", "fb_scalar_literal_beta",
      "fb_scalar_literal_gamma", "fb_scalar_pattern_alpha",
      "fb_scalar_pattern_beta"};
  const auto expected = FirebirdExpected();
  bool ok = true;
  ok &= ExpectDirectScalarResult(ParseStatement(sql), expected, markers,
                                 "firebird direct scalar expression");
  ok &= ExpectUdrScalarResult(sbu_firebird_parse_to_sblr(sql, trusted),
                              expected, markers,
                              "firebird UDR scalar expression");
  return ok;
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view sql =
      "select cast(null as datetime) as mysql_scalar_cast_alias,"
      "if(true and null is null, current_timestamp, "
      "date_add(current_date, interval 1 day)) as mysql_scalar_if_alias,"
      "coalesce(nullif('mysql_scalar_literal_alpha', "
      "'mysql_scalar_literal_beta'), ifnull(null, "
      "'mysql_scalar_literal_gamma')) as mysql_scalar_cond_alias,"
      "round(10 / 4) as mysql_scalar_numeric_alias "
      "where null <=> null or 'mysql_scalar_pattern_alpha' "
      "regexp 'mysql_scalar_pattern.*' or 'mysql_scalar_pattern_beta' "
      "like 'mysql_scalar_pattern%'";
  const std::initializer_list<std::string_view> markers = {
      "mysql_scalar_cast_alias", "mysql_scalar_if_alias",
      "mysql_scalar_cond_alias", "mysql_scalar_numeric_alias",
      "mysql_scalar_literal_alpha", "mysql_scalar_literal_beta",
      "mysql_scalar_literal_gamma", "mysql_scalar_pattern_alpha",
      "mysql_scalar_pattern_beta"};
  const auto expected = MysqlExpected();
  bool ok = true;
  ok &= ExpectDirectScalarResult(ParseStatement(sql), expected, markers,
                                 "mysql direct scalar expression");
  ok &= ExpectUdrScalarResult(sbu_mysql_parse_to_sblr(sql, trusted), expected,
                              markers, "mysql UDR scalar expression");
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::sbu_postgresql_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";
  constexpr std::string_view sql =
      "select cast(null as timestamp) as pg_scalar_cast_alias,"
      "case when true is distinct from false then current_timestamp + "
      "interval '1 day' else timestamp '2026-01-01' end "
      "as pg_scalar_case_alias,"
      "coalesce(nullif('pg_scalar_literal_alpha', "
      "'pg_scalar_literal_beta'), 'pg_scalar_literal_gamma') "
      "as pg_scalar_cond_alias,"
      "round(10::numeric / 4) as pg_scalar_numeric_alias "
      "where 'pg_scalar_pattern_alpha' similar to pg_scalar_similar_pattern "
      "or 'pg_scalar_pattern_beta' ~ 'pg_scalar_pattern.*'";
  const std::initializer_list<std::string_view> markers = {
      "pg_scalar_cast_alias", "pg_scalar_case_alias",
      "pg_scalar_cond_alias", "pg_scalar_numeric_alias",
      "pg_scalar_literal_alpha", "pg_scalar_literal_beta",
      "pg_scalar_literal_gamma", "pg_scalar_pattern_alpha",
      "pg_scalar_similar_pattern", "pg_scalar_pattern_beta", "2026-01-01"};
  const auto expected = PostgresqlExpected();
  bool ok = true;
  ok &= ExpectDirectScalarResult(ParseStatement(sql), expected, markers,
                                 "postgresql direct scalar expression");
  ok &= ExpectUdrScalarResult(sbu_postgresql_parse_to_sblr(sql, trusted),
                              expected, markers,
                              "postgresql UDR scalar expression");
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
