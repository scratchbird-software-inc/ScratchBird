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

struct StatisticsOptimizerExpected {
  std::string_view dialect;
  std::string_view release_profile;
  std::string_view donor_profile_uuid;
  std::string_view semantic_profile_uuid;
  std::string_view statistics_optimizer_profile;
  std::string_view statistics_optimizer_surface;
  std::string_view statistics_command_policy;
  std::string_view histogram_policy;
  std::string_view selectivity_policy;
  std::string_view stale_statistics_policy;
  std::string_view index_eligibility_policy;
  std::string_view plan_invalidation_policy;
  std::string_view analyze_command_policy;
  std::string_view explain_plan_policy;
  std::string_view catalog_projection_policy;
  std::string_view diagnostic_map_ref;
  std::string_view sandbox_root_policy;
  std::string_view statement_family;
  std::string_view operation_family;
  bool explain_surface{false};
  bool analyze_surface{false};
  bool statistics_update_surface{false};
  bool reindex_surface{false};
  bool optimize_surface{false};
  bool create_statistics_surface{false};
  bool drop_statistics_surface{false};
  bool index_statistics_surface{false};
  bool plan_query_surface{false};
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
                     " leaked statistics/optimizer source marker: " +
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
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready",
                    label);
  ok &= Expect(Contains(payload, "enterprise_implemented_proven\":false"),
               std::string(label) + " missing enterprise implementation proof");
  return ok;
}

bool ExpectEnvelopeAuthority(std::string_view payload,
                             const StatisticsOptimizerExpected& expected,
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

bool ExpectStatisticsOptimizerEvidence(
    std::string_view payload,
    const StatisticsOptimizerExpected& expected,
    std::string_view label) {
  bool ok = true;
  ok &= Expect(Contains(payload,
                        "\"statistics_optimizer_semantic_evidence\":{"),
               std::string(label) +
                   " missing statistics/optimizer semantic evidence");
  ok &= ExpectField(
      payload, "evidence_contract",
      "donor_statistics_optimizer_semantic_descriptor_evidence.v1", label);
  ok &= ExpectField(payload, "descriptor_resolution", "uuid_required", label);
  ok &= ExpectField(payload, "donor_profile_uuid",
                    expected.donor_profile_uuid, label);
  ok &= ExpectField(payload, "semantic_profile_uuid",
                    expected.semantic_profile_uuid, label);
  ok &= ExpectField(payload, "dialect", expected.dialect, label);
  ok &= ExpectField(payload, "release_profile", expected.release_profile,
                    label);
  ok &= ExpectField(payload, "statistics_optimizer_profile",
                    expected.statistics_optimizer_profile, label);
  ok &= ExpectField(payload, "statistics_optimizer_surface",
                    expected.statistics_optimizer_surface, label);
  ok &= ExpectField(payload, "statistics_command_policy",
                    expected.statistics_command_policy, label);
  ok &= ExpectField(payload, "histogram_policy", expected.histogram_policy,
                    label);
  ok &= ExpectField(payload, "selectivity_policy", expected.selectivity_policy,
                    label);
  ok &= ExpectField(payload, "stale_statistics_policy",
                    expected.stale_statistics_policy, label);
  ok &= ExpectField(payload, "index_eligibility_policy",
                    expected.index_eligibility_policy, label);
  ok &= ExpectField(payload, "plan_invalidation_policy",
                    expected.plan_invalidation_policy, label);
  ok &= ExpectField(payload, "analyze_command_policy",
                    expected.analyze_command_policy, label);
  ok &= ExpectField(payload, "explain_plan_policy",
                    expected.explain_plan_policy, label);
  ok &= ExpectField(payload, "catalog_projection_policy",
                    expected.catalog_projection_policy, label);
  ok &= ExpectField(payload, "diagnostic_map_ref",
                    expected.diagnostic_map_ref, label);
  ok &= ExpectField(payload, "sandbox_root_policy",
                    expected.sandbox_root_policy, label);
  ok &= ExpectBool(payload, "explain_surface", expected.explain_surface, label);
  ok &= ExpectBool(payload, "analyze_surface", expected.analyze_surface, label);
  ok &= ExpectBool(payload, "statistics_update_surface",
                   expected.statistics_update_surface, label);
  ok &= ExpectBool(payload, "reindex_surface", expected.reindex_surface, label);
  ok &= ExpectBool(payload, "optimize_surface", expected.optimize_surface,
                   label);
  ok &= ExpectBool(payload, "create_statistics_surface",
                   expected.create_statistics_surface, label);
  ok &= ExpectBool(payload, "drop_statistics_surface",
                   expected.drop_statistics_surface, label);
  ok &= ExpectBool(payload, "index_statistics_surface",
                   expected.index_statistics_surface, label);
  ok &= ExpectBool(payload, "plan_query_surface", expected.plan_query_surface,
                   label);
  ok &= ExpectBool(payload, "uuid_required_semantic_profile", true, label);
  ok &= ExpectBool(payload, "catalog_descriptor_required", true, label);
  ok &= ExpectBool(payload, "statistics_descriptor_required", true, label);
  ok &= ExpectBool(payload, "optimizer_descriptor_required", true, label);
  ok &= ExpectBool(payload, "sblr_operation_uuid_resolution_required", true,
                   label);
  ok &= ExpectField(payload, "engine_authority", "scratchbird", label);
  ok &= ExpectField(payload, "statistics_authority",
                    "engine_statistics_descriptor_authority", label);
  ok &= ExpectField(payload, "optimizer_authority",
                    "engine_optimizer_authority", label);
  ok &= ExpectField(payload, "histogram_authority",
                    "engine_statistics_descriptor_authority", label);
  ok &= ExpectField(payload, "selectivity_authority",
                    "engine_statistics_descriptor_authority", label);
  ok &= ExpectField(payload, "stale_statistics_authority",
                    "engine_statistics_descriptor_epoch", label);
  ok &= ExpectField(payload, "index_eligibility_authority",
                    "engine_index_descriptor_authority", label);
  ok &= ExpectField(payload, "plan_invalidation_authority",
                    "engine_optimizer_catalog_epoch", label);
  ok &= ExpectField(payload, "catalog_projection_authority",
                    "engine_catalog_uuid_projection", label);
  ok &= ExpectBool(payload, "source_sql_text_included", false, label);
  ok &= ExpectBool(payload, "literal_text_included", false, label);
  ok &= ExpectBool(payload, "object_name_text_included", false, label);
  ok &= ExpectBool(payload, "quoted_identifier_text_included", false, label);
  ok &= ExpectBool(payload, "sblr_embeds_source_identifiers", false, label);
  ok &= ExpectBool(payload, "parser_statistics_authority", false, label);
  ok &= ExpectBool(payload, "parser_optimizer_authority", false, label);
  ok &= ExpectBool(payload, "parser_histogram_authority", false, label);
  ok &= ExpectBool(payload, "parser_selectivity_authority", false, label);
  ok &= ExpectBool(payload, "parser_stale_statistics_authority", false, label);
  ok &= ExpectBool(payload, "parser_index_eligibility_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_plan_invalidation_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_catalog_authority", false, label);
  ok &= ExpectBool(payload, "parser_storage_authority", false, label);
  ok &= ExpectBool(payload, "parser_execution_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_authority", false, label);
  ok &= ExpectBool(payload, "parser_transaction_finality_authority", false,
                   label);
  ok &= ExpectBool(payload, "parser_runtime_semantic_equivalence_authority",
                   false, label);
  ok &= ExpectBool(payload, "donor_sql_executed", false, label);
  ok &= ExpectField(payload, "runtime_semantic_equivalence",
                    "not_enterprise_proven_pending", label);
  ok &= ExpectField(
      payload, "descriptor_exactness_status",
      "parser_statistics_optimizer_descriptor_recorded_runtime_equivalence_pending",
      label);
  ok &= ExpectField(payload, "enterprise_readiness", "not_enterprise_ready",
                    label);
  ok &= Expect(!Contains(payload, "parser_statistics_authority\":true"),
               std::string(label) + " granted parser statistics authority");
  ok &= Expect(!Contains(payload, "parser_optimizer_authority\":true"),
               std::string(label) + " granted parser optimizer authority");
  ok &= Expect(!Contains(payload, "parser_plan_invalidation_authority\":true"),
               std::string(label) +
                   " granted parser plan invalidation authority");
  ok &= Expect(!Contains(payload, "donor_sql_executed\":true"),
               std::string(label) + " executed donor SQL");
  return ok;
}

template <typename Result>
bool ExpectDirectResult(const Result& result,
                        const StatisticsOptimizerExpected& expected,
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
  ok &= ExpectStatisticsOptimizerEvidence(result.parser_evidence_json, expected,
                                          label);
  ok &= ExpectEnvelopeAuthority(result.sblr_envelope, expected, label);
  ok &= ExpectStatisticsOptimizerEvidence(result.sblr_envelope, expected,
                                          label);
  ok &= ExpectNoMarkers(result.parser_evidence_json, markers, label);
  ok &= ExpectNoMarkers(result.sblr_envelope, markers, label);
  ok &= ExpectNoMysqlLts(result.parser_evidence_json, label);
  ok &= ExpectNoMysqlLts(result.sblr_envelope, label);
  return ok;
}

template <typename UdrResult>
bool ExpectUdrResult(const UdrResult& result,
                     const StatisticsOptimizerExpected& expected,
                     std::span<const std::string_view> markers,
                     std::string_view label) {
  bool ok = true;
  ok &= Expect(result.ok, std::string(label) + " UDR parse failed: " +
                             result.message_vector_json);
  ok &= ExpectEnvelopeAuthority(result.payload, expected, label);
  ok &= ExpectStatisticsOptimizerEvidence(result.payload, expected, label);
  ok &= ExpectNoMarkers(result.payload, markers, label);
  ok &= ExpectNoMysqlLts(result.payload, label);
  return ok;
}

template <typename DirectParser, typename UdrParser>
bool RunCase(DirectParser direct_parser,
             UdrParser udr_parser,
             std::string_view sql,
             const StatisticsOptimizerExpected& expected,
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
  static constexpr std::string_view markers[] = {"fb_stats_marker",
                                                 "FB_STATS_MARKER"};
  return markers;
}

std::span<const std::string_view> MysqlMarkers() {
  static constexpr std::string_view markers[] = {"mysql_stats_marker",
                                                 "MYSQL_STATS_MARKER"};
  return markers;
}

std::span<const std::string_view> PostgresqlMarkers() {
  static constexpr std::string_view markers[] = {"pg_stats_marker",
                                                 "PG_STATS_MARKER"};
  return markers;
}

bool FirebirdChecks() {
  using scratchbird::parser::firebird::ParseStatement;
  using scratchbird::udr::firebird_parser_support::sbu_firebird_parse_to_sblr;
  return RunCase(
      ParseStatement, sbu_firebird_parse_to_sblr,
      "set statistics index fb_stats_marker",
      {"firebird",
       "5.0.4",
       "019e13c0-0000-7000-8000-000000000302",
       "019e13c0-1b00-7000-8000-000000000302",
       "firebird.statistics_optimizer_metadata_semantics_profile",
       "firebird_set_statistics_index_selectivity_descriptor",
       "firebird_set_statistics_index_descriptor_only_engine_recomputes_selectivity",
       "firebird_index_selectivity_descriptor_engine_statistics_authority",
       "firebird_rdb_index_statistics_selectivity_descriptor_engine_authority",
       "firebird_stale_statistics_recompute_requires_engine_statistics_epoch",
       "firebird_index_selectivity_eligibility_engine_index_descriptor",
       "firebird_plan_invalidation_engine_metadata_statistics_epoch",
       "firebird_set_statistics_index_maps_to_engine_statistics_descriptor_request",
       "firebird_plan_metadata_descriptor_only_no_parser_optimizer_authority",
       "firebird_rdb_indices_statistics_catalog_projection_uuid_required",
       "firebird_statistics_optimizer_semantics_diagnostic_map",
       "firebird_donor_schema_root_uuid_required_no_cross_root_temp_access",
       "optimizer",
       "firebird.statistics.set_index_statistics",
       false, true, true, false, false, false, false, true, false},
      FirebirdMarkers(), "firebird statistics optimizer descriptor");
}

bool MysqlChecks() {
  using scratchbird::parser::mysql::ParseStatement;
  using scratchbird::udr::mysql_parser_support::sbu_mysql_parse_to_sblr;
  return RunCase(
      ParseStatement, sbu_mysql_parse_to_sblr,
      "explain select * from mysql_stats_marker where id = 1",
      {"mysql",
       "9.7.0",
       "019e13c0-0000-7000-8000-000000000303",
       "019e13c0-1b00-7000-8000-000000000303",
       "mysql.statistics_optimizer_metadata_semantics_profile",
       "mysql_explain_plan_catalog_projection_descriptor",
       "mysql_optimizer_metadata_catalog_projection_only",
       "mysql_histogram_descriptor_engine_statistics_authority",
       "mysql_index_cardinality_selectivity_descriptor_engine_authority",
       "mysql_persistent_statistics_staleness_descriptor_engine_epoch",
       "mysql_visible_index_optimizer_eligibility_engine_descriptor",
       "mysql_plan_invalidation_engine_dictionary_statistics_epoch",
       "mysql_analyze_table_policy_descriptor_required",
       "mysql_explain_catalog_projection_descriptor_no_plan_authority",
       "mysql_information_schema_statistics_projection_uuid_required",
       "mysql_statistics_optimizer_semantics_diagnostic_map",
       "mysql_connected_database_root_uuid_required_temp_shadowing_root_local",
       "optimizer",
       "mysql.optimizer.explain",
       true, false, false, false, false, false, false, false, true},
      MysqlMarkers(), "mysql statistics optimizer descriptor");
}

bool PostgresqlChecks() {
  using scratchbird::parser::postgresql::ParseStatement;
  using scratchbird::udr::postgresql_parser_support::
      sbu_postgresql_parse_to_sblr;
  return RunCase(
      ParseStatement, sbu_postgresql_parse_to_sblr,
      "analyze pg_stats_marker",
      {"postgresql",
       "18.3",
       "019e13c0-0000-7000-8000-000000000304",
       "019e13c0-1b00-7000-8000-000000000304",
       "postgresql.statistics_optimizer_metadata_semantics_profile",
       "postgresql_analyze_statistics_update_refused_descriptor",
       "postgresql_statistics_maintenance_command_refused_no_donor_execution",
       "postgresql_pg_statistic_histogram_descriptor_engine_statistics_authority",
       "postgresql_ndistinct_mcv_selectivity_descriptor_engine_authority",
       "postgresql_autovacuum_analyze_staleness_descriptor_engine_epoch",
       "postgresql_index_valid_ready_predicate_eligibility_engine_descriptor",
       "postgresql_plan_cache_invalidation_engine_catalog_statistics_epoch",
       "postgresql_analyze_refused_descriptor_no_donor_execution",
       "postgresql_explain_policy_descriptor_required",
       "postgresql_pg_statistic_pg_stats_projection_uuid_required",
       "postgresql_statistics_optimizer_semantics_diagnostic_map",
       "postgresql_connected_database_schema_root_uuid_required_pg_temp_root_local",
       "maintenance",
       "postgresql.maintenance.analyze",
       false, true, true, false, false, false, false, false, false},
      PostgresqlMarkers(), "postgresql statistics optimizer descriptor");
}

} // namespace

int main() {
  bool ok = true;
  ok &= FirebirdChecks();
  ok &= MysqlChecks();
  ok &= PostgresqlChecks();
  if (!ok) return EXIT_FAILURE;
  return EXIT_SUCCESS;
}
