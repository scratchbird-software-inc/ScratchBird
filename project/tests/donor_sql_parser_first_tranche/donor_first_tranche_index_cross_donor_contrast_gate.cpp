// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "firebird_dialect.hpp"
#include "postgresql_dialect.hpp"
#include "sbu_firebird_parser_support.hpp"
#include "sbu_postgresql_parser_support.hpp"

#include <cstdlib>
#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>

namespace {

struct IndexDescriptorExpectation {
  std::string_view dialect;
  std::string_view operation_family;
  std::string_view release_profile;
  std::string_view donor_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view index_profile;
  std::string_view ddl_surface;
  std::string_view index_method;
  std::string_view unique_null_policy;
  std::string_view null_ordering;
  std::string_view collation_policy;
  std::string_view operator_family_policy;
  std::string_view predicate_or_expression_policy;
  std::string_view validation_state;
  std::string_view build_mode;
  std::string_view statistics_policy_ref;
  bool nulls_not_distinct_requested{false};
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
                std::string(label) + " missing bool " + std::string(field) +
                    "=" + std::string(value ? "true" : "false"));
}

bool ExpectNoMarkers(std::string_view payload,
                     std::initializer_list<std::string_view> markers,
                     std::string_view label) {
  bool ok = true;
  for (const auto marker : markers) {
    ok &= Expect(!Contains(payload, marker),
                 std::string(label) + " leaked marker " +
                     std::string(marker));
  }
  return ok;
}

bool ExpectEnterprisePending(std::string_view payload, std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"enterprise_readiness_evidence\":{"),
               std::string(label) + " missing enterprise readiness evidence");
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
                             const IndexDescriptorExpectation& expected,
                             std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"envelope\":\"SBLRExecutionEnvelope.v3\""),
               std::string(label) + " missing SBLR envelope v3");
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "operation_family", expected.operation_family,
                    label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectBool(payload, "donor_engine_sql_executed", false, label);
  ok &= ExpectBool(payload, "sql_text_included", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false,
                   label);
  ok &= ExpectEnterprisePending(payload, label);
  return ok;
}

bool ExpectIndexDescriptor(std::string_view payload,
                           const IndexDescriptorExpectation& expected,
                           std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload, "\"index_semantic_defaults_evidence\":{"),
               std::string(label) + " missing index semantic evidence");
  ok &= ExpectField(payload, "evidence_contract",
                    "donor_index_semantic_defaults_evidence.v1", label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "donor_profile_uuid", expected.donor_profile_uuid,
                    label);
  ok &= ExpectField(payload, "semantic_profile_uuid",
                    expected.semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "release_profile", expected.release_profile,
                    label);
  ok &= ExpectField(payload, "index_profile", expected.index_profile, label);
  ok &= ExpectField(payload, "ddl_surface", expected.ddl_surface, label);
  ok &= ExpectField(payload, "index_method", expected.index_method, label);
  ok &= ExpectBool(payload, "unique_requested", true, label);
  ok &= ExpectField(payload, "unique_null_policy",
                    expected.unique_null_policy, label);
  ok &= ExpectField(payload, "null_ordering", expected.null_ordering, label);
  ok &= ExpectField(payload, "collation_policy", expected.collation_policy,
                    label);
  ok &= ExpectField(payload, "operator_family_policy",
                    expected.operator_family_policy, label);
  ok &= ExpectField(payload, "predicate_or_expression_policy",
                    expected.predicate_or_expression_policy, label);
  ok &= ExpectField(payload, "validation_state", expected.validation_state,
                    label);
  ok &= ExpectField(payload, "build_mode", expected.build_mode, label);
  ok &= ExpectField(payload, "statistics_policy_ref",
                    expected.statistics_policy_ref, label);
  ok &= ExpectBool(payload, "nulls_not_distinct_requested",
                   expected.nulls_not_distinct_requested, label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "generic_index_default_allowed", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "donor_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready",
                    label);
  return ok;
}

bool ExpectCrossDonorDistinctFields(
    const IndexDescriptorExpectation& firebird,
    const IndexDescriptorExpectation& postgresql) {
  bool ok = true;
  auto distinct = [&ok](std::string_view field, std::string_view left,
                       std::string_view right) {
    ok &= Expect(left != right,
                 "Firebird/PostgreSQL index contrast did not differ for " +
                     std::string(field));
  };
  distinct("donor_profile_uuid", firebird.donor_profile_uuid,
           postgresql.donor_profile_uuid);
  distinct("semantic_profile_uuid", firebird.semantic_profile_uuid,
           postgresql.semantic_profile_uuid);
  distinct("dialect", firebird.dialect, postgresql.dialect);
  distinct("index_profile", firebird.index_profile, postgresql.index_profile);
  distinct("unique_null_policy", firebird.unique_null_policy,
           postgresql.unique_null_policy);
  distinct("null_ordering", firebird.null_ordering,
           postgresql.null_ordering);
  distinct("operator_family_policy", firebird.operator_family_policy,
           postgresql.operator_family_policy);
  distinct("predicate_or_expression_policy",
           firebird.predicate_or_expression_policy,
           postgresql.predicate_or_expression_policy);
  distinct("validation_state", firebird.validation_state,
           postgresql.validation_state);
  distinct("build_mode", firebird.build_mode, postgresql.build_mode);
  distinct("statistics_policy_ref", firebird.statistics_policy_ref,
           postgresql.statistics_policy_ref);
  return ok;
}

bool ExpectFirebirdLeakageGuard(std::string_view payload,
                                std::string_view label) {
  return ExpectNoMarkers(payload,
                         {"postgresql", "postgresql_", "postgresql.",
                          "mysql_lts", "\"dialect\":\"mysql_lts\""},
                         label);
}

bool ExpectPostgresqlLeakageGuard(std::string_view payload,
                                  std::string_view label) {
  return ExpectNoMarkers(payload,
                         {"firebird", "firebird_", "firebird.", "mysql_lts",
                          "\"dialect\":\"mysql_lts\""},
                         label);
}

template <typename Result, typename LeakageGuard>
bool ExpectDirectIndexResult(const Result& result,
                             const IndexDescriptorExpectation& expected,
                             std::initializer_list<std::string_view> sql_markers,
                             std::string_view label,
                             LeakageGuard leakage_guard) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " did not parse: " +
                             result.message_vector_json);
  ok &= Expect(result.operation_family == expected.operation_family,
               std::string(label) + " operation family mismatch: " +
                   result.operation_family);
  ok &= ExpectField(result.parser_evidence_json, "statement_kind",
                    "CREATE_INDEX", label);
  ok &= ExpectBool(result.parser_evidence_json, "source_text_redacted", true,
                   label);
  ok &= ExpectBool(result.parser_evidence_json, "descriptor_uuid_required",
                   true, label);
  ok &= ExpectBool(result.parser_evidence_json,
                   "parser_transaction_finality_authority", false, label);
  ok &= ExpectBool(result.parser_evidence_json, "parser_storage_authority",
                   false, label);
  ok &= ExpectIndexDescriptor(result.parser_evidence_json, expected, label);
  ok &= ExpectEnvelopeAuthority(result.sblr_envelope, expected, label);
  ok &= ExpectIndexDescriptor(result.sblr_envelope, expected, label);
  ok &= leakage_guard(result.parser_evidence_json, label);
  ok &= leakage_guard(result.sblr_envelope, label);
  ok &= ExpectNoMarkers(result.parser_evidence_json, sql_markers, label);
  ok &= ExpectNoMarkers(result.sblr_envelope, sql_markers, label);
  return ok;
}

template <typename UdrResult, typename LeakageGuard>
bool ExpectUdrIndexResult(const UdrResult& result,
                          const IndexDescriptorExpectation& expected,
                          std::initializer_list<std::string_view> sql_markers,
                          std::string_view label,
                          LeakageGuard leakage_guard) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelopeAuthority(result.payload, expected, label);
  ok &= ExpectIndexDescriptor(result.payload, expected, label);
  ok &= leakage_guard(result.payload, label);
  ok &= ExpectNoMarkers(result.payload, sql_markers, label);
  return ok;
}

constexpr IndexDescriptorExpectation kFirebirdUniqueIndex{
    "firebird",
    "firebird.ddl.create.unique_index",
    "5.0.4",
    "019e13c0-0000-7000-8000-000000000302",
    "019e13c0-1000-7000-8000-000000000302",
    "firebird.index_optimizer_translation_profile",
    "create_unique_index",
    "firebird_btree_ascending_index_profile",
    "firebird_unique_index_nulls_are_distinct_profile",
    "firebird_nulls_first_for_ascending_index_profile",
    "firebird_column_charset_collation_descriptor",
    "firebird_builtin_comparison_no_named_operator_family",
    "firebird_column_index_no_partial_predicate",
    "firebird_index_active_default",
    "firebird_immediate_index_build_default",
    "firebird_index_selectivity_statistics_profile",
    false};

constexpr IndexDescriptorExpectation kPostgresqlUniqueIndex{
    "postgresql",
    "postgresql.ddl.create.unique_index",
    "18.3",
    "019e13c0-0000-7000-8000-000000000304",
    "019e13c0-1000-7000-8000-000000000304",
    "postgresql.index_optimizer_translation_profile",
    "create_unique_index",
    "postgresql_btree_access_method_default",
    "postgresql_unique_nulls_distinct_default",
    "postgresql_nulls_last_for_ascending_btree_default",
    "postgresql_per_expression_collation_descriptor",
    "postgresql_default_operator_class_and_family_resolution",
    "postgresql_column_index_no_predicate_descriptor",
    "postgresql_index_valid_after_build_default",
    "postgresql_nonconcurrent_index_build_default",
    "postgresql_pg_statistic_and_pg_class_index_statistics_profile",
    false};

constexpr IndexDescriptorExpectation kPostgresqlNullsNotDistinctIndex{
    "postgresql",
    "postgresql.ddl.create.unique_index",
    "18.3",
    "019e13c0-0000-7000-8000-000000000304",
    "019e13c0-1000-7000-8000-000000000304",
    "postgresql.index_optimizer_translation_profile",
    "create_unique_index",
    "postgresql_btree_access_method_default",
    "postgresql_unique_nulls_not_distinct_requested",
    "postgresql_nulls_last_for_ascending_btree_default",
    "postgresql_per_expression_collation_descriptor",
    "postgresql_default_operator_class_and_family_resolution",
    "postgresql_column_index_no_predicate_descriptor",
    "postgresql_index_valid_after_build_default",
    "postgresql_nonconcurrent_index_build_default",
    "postgresql_pg_statistic_and_pg_class_index_statistics_profile",
    true};

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";

  bool ok = true;
  ok &= ExpectDirectIndexResult(
      ParseStatement("create unique index fb_idx on fb_t (code)"),
      kFirebirdUniqueIndex, {"fb_idx"},
      "firebird direct unique btree index", ExpectFirebirdLeakageGuard);
  ok &= ExpectUdrIndexResult(
      sbu_firebird_parse_to_sblr(
          "create unique index fb_udr_idx on fb_udr_t (code)", trusted),
      kFirebirdUniqueIndex, {"fb_udr_idx"},
      "firebird UDR unique btree index", ExpectFirebirdLeakageGuard);
  return ok;
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::
      sbu_postgresql_parse_to_sblr;
  constexpr std::string_view trusted = "engine_context=trusted;resolver=uuid";

  bool ok = true;
  ok &= ExpectDirectIndexResult(
      ParseStatement("create unique index pg_idx on pg_t (code)"),
      kPostgresqlUniqueIndex, {"pg_idx"},
      "postgresql direct unique btree index", ExpectPostgresqlLeakageGuard);
  ok &= ExpectUdrIndexResult(
      sbu_postgresql_parse_to_sblr(
          "create unique index pg_udr_idx on pg_udr_t (code)", trusted),
      kPostgresqlUniqueIndex, {"pg_udr_idx"},
      "postgresql UDR unique btree index", ExpectPostgresqlLeakageGuard);
  ok &= ExpectDirectIndexResult(
      ParseStatement(
          "create unique index pg_idx2 on pg_t2 (code) nulls not distinct"),
      kPostgresqlNullsNotDistinctIndex, {"pg_idx2"},
      "postgresql direct unique btree nulls-not-distinct index",
      ExpectPostgresqlLeakageGuard);
  ok &= ExpectUdrIndexResult(
      sbu_postgresql_parse_to_sblr(
          "create unique index pg_udr_idx2 on pg_udr_t2 (code) nulls not distinct",
          trusted),
      kPostgresqlNullsNotDistinctIndex, {"pg_udr_idx2"},
      "postgresql UDR unique btree nulls-not-distinct index",
      ExpectPostgresqlLeakageGuard);
  return ok;
}

} // namespace

int main() {
  bool ok = true;
  ok &= ExpectCrossDonorDistinctFields(kFirebirdUniqueIndex,
                                       kPostgresqlUniqueIndex);
  ok &= FirebirdChecks();
  ok &= PostgresqlChecks();
  return ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
