// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/nosql_statistics_api.hpp"

#include <cstdlib>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

api::EngineRequestContext Context() {
  api::EngineRequestContext context;
  context.database_path = "/tmp/sb_odf_078_gate_api.sbdb";
  context.database_uuid.canonical = "019df078-0000-7000-8000-000000000001";
  context.transaction_uuid.canonical = "019df078-0000-7000-8000-000000000078";
  context.local_transaction_id = 78;
  context.security_context_present = true;
  return context;
}

api::EngineNoSqlStatisticInput Stat(api::EngineNoSqlProviderFamily family,
                                    std::string kind,
                                    std::string key,
                                    api::EngineApiU64 count,
                                    api::EngineApiU64 distinct_values,
                                    api::EngineApiU64 bucket_count = 0,
                                    api::EngineApiU64 vector_dimension = 0) {
  api::EngineNoSqlStatisticInput stat;
  stat.family = family;
  stat.statistic_kind = std::move(kind);
  stat.statistic_key = std::move(key);
  stat.count = count;
  stat.distinct_values = distinct_values;
  stat.bucket_count = bucket_count;
  stat.vector_dimension = vector_dimension;
  stat.fresh = true;
  stat.authoritative = true;
  stat.physical_provider_backed = true;
  return stat;
}

std::vector<api::EngineNoSqlStatisticInput> AllFamilyStats() {
  using Family = api::EngineNoSqlProviderFamily;
  return {
      Stat(Family::kKeyValue, "key_cardinality", "tenant", 1200, 1200),
      Stat(Family::kKeyValue, "prefix_frequency", "tenant:us", 700, 1),
      Stat(Family::kKeyValue, "ttl_active_count", "ttl.active", 1100, 1),
      Stat(Family::kKeyValue, "ttl_expired_count", "ttl.expired", 100, 1),

      Stat(Family::kDocument, "path_cardinality", "$.customer.id", 1000, 930),
      Stat(Family::kDocument, "wildcard_path_frequency", "$.items[*].sku", 620, 18),
      Stat(Family::kDocument, "shape_dictionary_frequency", "shape.invoice", 450, 12),
      Stat(Family::kDocument, "shape_dictionary_ndv", "shape.ndv", 450, 12),

      Stat(Family::kSearch, "token_frequency", "refund", 340, 1),
      Stat(Family::kSearch, "document_frequency", "refund", 225, 1),
      Stat(Family::kSearch, "segment_count", "sealed", 9, 9),
      Stat(Family::kSearch, "term_distribution", "terms", 12000, 8700, 64),

      Stat(Family::kVector, "vector_count", "embedding", 4096, 4096, 0, 384),
      Stat(Family::kVector, "dimension_distribution", "dims.384", 4096, 1, 4, 384),
      Stat(Family::kVector, "metadata_filter_frequency", "region=us", 2100, 1),
      Stat(Family::kVector, "sparse_term_frequency", "hybrid.refund", 180, 1),

      Stat(Family::kGraph, "vertex_cardinality", "vertex", 800, 800),
      Stat(Family::kGraph, "edge_cardinality", "edge", 3200, 3200),
      Stat(Family::kGraph, "label_frequency", "Account", 500, 1),
      Stat(Family::kGraph, "property_frequency", "risk_score", 430, 12),
      Stat(Family::kGraph, "degree_distribution", "out_degree", 3200, 18, 16),
      Stat(Family::kGraph, "frontier_distribution", "frontier.depth2", 900, 24, 8),

      Stat(Family::kTimeSeries, "metric_cardinality", "cpu.usage", 730, 90),
      Stat(Family::kTimeSeries, "bucket_count", "hour", 168, 168),
      Stat(Family::kTimeSeries, "meta_key_frequency", "host", 730, 44),
      Stat(Family::kTimeSeries, "rollup_distribution", "rollup.5m", 2016, 12, 24),
  };
}

api::EnginePlanNoSqlStatisticsAdvisorRequest BaseRequest() {
  api::EnginePlanNoSqlStatisticsAdvisorRequest request;
  request.context = Context();
  request.target_object.uuid.canonical =
      "019df078-0000-7000-8000-0000000000aa";
  request.target_object.object_kind = "nosql_collection";
  request.statistics = AllFamilyStats();
  request.stats_epoch = 44;
  request.required_stats_epoch = 44;
  request.catalog_epoch = 55;
  request.security_epoch = 66;
  request.policy_epoch = 77;
  request.stats_visibility_epoch = 88;
  request.stats_catalog_authoritative = true;
  request.stats_are_fresh = true;
  request.security_redaction_proof_present = true;
  request.security_snapshot_bound = true;
  request.grants_proven = true;
  request.engine_mga_authoritative = true;
  request.mga_recheck_required = true;
  request.security_recheck_required = true;
  request.candidate_benefit_score = 125;
  request.promotion_benefit_threshold = 100;
  return request;
}

std::string RowField(const api::EngineApiResult& result,
                     std::size_t row_index,
                     std::string_view field) {
  if (row_index >= result.result_shape.rows.size()) {
    return {};
  }
  for (const auto& [name, value] : result.result_shape.rows[row_index].fields) {
    if (name == field) {
      return value.encoded_value;
    }
  }
  return {};
}

bool RowMatches(const api::EngineApiResult& result,
                std::initializer_list<std::pair<std::string_view,
                                                std::string_view>> fields) {
  for (std::size_t i = 0; i < result.result_shape.rows.size(); ++i) {
    bool matched = true;
    for (const auto& [field, expected] : fields) {
      if (RowField(result, i, field) != expected) {
        matched = false;
        break;
      }
    }
    if (matched) {
      return true;
    }
  }
  return false;
}

bool DiagnosticContains(const api::EngineApiResult& result,
                        std::string_view token) {
  for (const auto& diagnostic : result.diagnostics) {
    if (diagnostic.code.find(token) != std::string::npos ||
        diagnostic.detail.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

bool EvidenceContains(const api::EngineApiResult& result,
                      std::string_view kind,
                      std::string_view id) {
  for (const auto& item : result.evidence) {
    if (item.evidence_kind.find(kind) != std::string::npos &&
        item.evidence_id.find(id) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireEvidenceHygiene(const api::EngineApiResult& result) {
  std::vector<std::string> values;
  for (const auto& item : result.evidence) {
    values.push_back(item.evidence_kind);
    values.push_back(item.evidence_id);
  }
  for (const auto& diagnostic : result.diagnostics) {
    values.push_back(diagnostic.code);
    values.push_back(diagnostic.message_key);
    values.push_back(diagnostic.detail);
  }
  for (std::size_t i = 0; i < result.result_shape.rows.size(); ++i) {
    for (const auto& [name, value] : result.result_shape.rows[i].fields) {
      values.push_back(name);
      values.push_back(value.encoded_value);
    }
  }
  for (const auto& value : values) {
    for (const auto forbidden : {"docs/", "execution-plans", "findings",
                                 "contracts", "references",
                                 "behavior_store_scan_selected=true",
                                 "descriptor_scan_selected=true",
                                 "provider_transaction_finality_authority=true",
                                 "provider_visibility_authority=true",
                                 "parser_transaction_finality_authority=true",
                                 "client_visibility_or_finality_authority=true",
                                 "client_autocommit_authority=true",
                                 "write_ahead_log_finality_authority=true"}) {
      Require(value.find(forbidden) == std::string::npos,
              "ODF-078 evidence leaked forbidden documentation or authority token");
    }
  }
}

void AllSixFamiliesEmitFamilySpecificStatsAndInvisibleCandidates() {
  const auto result = api::EnginePlanNoSqlStatisticsAdvisor(BaseRequest());
  Require(result.ok, "ODF-078 authoritative NoSQL stats/advisor plan failed");
  Require(result.dml_summary.benchmark_clean,
          "ODF-078 result was not benchmark clean");
  Require(result.dml_summary.visible_rows_scanned == 0,
          "ODF-078 used descriptor or behavior scans as stats authority");
  Require(result.advisor_result.statistics_rows.size() == 26,
          "ODF-078 did not emit every family-specific statistics row");
  Require(result.advisor_result.candidates.size() == 6,
          "ODF-078 did not build six adaptive index candidates");
  Require(result.advisor_result.candidate_invisible,
          "ODF-078 initial candidates were not invisible");
  Require(EvidenceContains(result, "nosql_surface", "statistics_advisor"),
          "ODF-078 missing NoSQL surface evidence");
  Require(EvidenceContains(result, "nosql_statistics_advisor",
                           "catalog_statistics_authority=authoritative_physical_stats"),
          "ODF-078 missing physical stats catalog authority evidence");
  Require(EvidenceContains(result, "parser_executes_sql", "false"),
          "ODF-078 parser SQL authority guard evidence missing");
  Require(EvidenceContains(result, "wal_recovery_authority", "false"),
          "ODF-078 WAL authority guard evidence missing");
  Require(EvidenceContains(result, "provider_transaction_finality_authority",
                           "false"),
          "ODF-078 provider finality guard evidence missing");
  Require(EvidenceContains(result, "provider_visibility_authority", "false"),
          "ODF-078 provider visibility guard evidence missing");
  Require(EvidenceContains(result, "row_mga_recheck_evidence", "required"),
          "ODF-078 row MGA recheck evidence missing");
  Require(EvidenceContains(result, "row_security_recheck_evidence", "required"),
          "ODF-078 row security recheck evidence missing");

  const struct {
    const char* family;
    const char* statistic_kind;
    const char* index_kind;
  } expected[] = {
      {"key_value", "key_cardinality", "kv_prefix_ttl_adaptive_index"},
      {"key_value", "prefix_frequency", "kv_prefix_ttl_adaptive_index"},
      {"key_value", "ttl_active_count", "kv_prefix_ttl_adaptive_index"},
      {"key_value", "ttl_expired_count", "kv_prefix_ttl_adaptive_index"},
      {"document", "path_cardinality", "document_path_shape_adaptive_index"},
      {"document", "wildcard_path_frequency", "document_path_shape_adaptive_index"},
      {"document", "shape_dictionary_frequency", "document_path_shape_adaptive_index"},
      {"document", "shape_dictionary_ndv", "document_path_shape_adaptive_index"},
      {"search", "token_frequency", "search_term_segment_adaptive_index"},
      {"search", "document_frequency", "search_term_segment_adaptive_index"},
      {"search", "segment_count", "search_term_segment_adaptive_index"},
      {"search", "term_distribution", "search_term_segment_adaptive_index"},
      {"vector", "vector_count", "vector_metadata_sparse_adaptive_index"},
      {"vector", "dimension_distribution", "vector_metadata_sparse_adaptive_index"},
      {"vector", "metadata_filter_frequency", "vector_metadata_sparse_adaptive_index"},
      {"vector", "sparse_term_frequency", "vector_metadata_sparse_adaptive_index"},
      {"graph", "vertex_cardinality", "graph_label_degree_adaptive_index"},
      {"graph", "edge_cardinality", "graph_label_degree_adaptive_index"},
      {"graph", "label_frequency", "graph_label_degree_adaptive_index"},
      {"graph", "property_frequency", "graph_label_degree_adaptive_index"},
      {"graph", "degree_distribution", "graph_label_degree_adaptive_index"},
      {"graph", "frontier_distribution", "graph_label_degree_adaptive_index"},
      {"time_series", "metric_cardinality", "time_series_metric_bucket_adaptive_index"},
      {"time_series", "bucket_count", "time_series_metric_bucket_adaptive_index"},
      {"time_series", "meta_key_frequency", "time_series_metric_bucket_adaptive_index"},
      {"time_series", "rollup_distribution", "time_series_metric_bucket_adaptive_index"},
  };

  std::set<std::string> families;
  for (const auto& item : expected) {
    Require(RowMatches(result,
                       {{"row_kind", "family_statistic"},
                        {"family", item.family},
                        {"statistic_kind", item.statistic_kind}}),
            "ODF-078 missing expected family-specific statistic");
    Require(RowMatches(result,
                       {{"row_kind", "adaptive_index_candidate"},
                        {"family", item.family},
                        {"index_kind", item.index_kind},
                        {"promotion_state", "invisible"},
                        {"invisible", "true"},
                        {"promoted_visible", "false"}}),
            "ODF-078 missing invisible adaptive index candidate");
    families.insert(item.family);
  }
  Require(families.size() == 6,
          "ODF-078 did not cover all six physical NoSQL families");
  RequireEvidenceHygiene(result);
}

void PromotionSucceedsOnlyAfterRequiredProof() {
  auto request = BaseRequest();
  request.promotion_requested = true;
  auto result = api::EnginePlanNoSqlStatisticsAdvisor(request);
  Require(result.ok, "ODF-078 promotion with full proof failed");
  Require(result.advisor_result.promotion_succeeded,
          "ODF-078 full-proof promotion did not succeed");
  Require(RowMatches(result,
                     {{"row_kind", "adaptive_index_candidate"},
                      {"family", "vector"},
                      {"promotion_state", "visible_active"},
                      {"invisible", "false"},
                      {"promoted_visible", "true"}}),
          "ODF-078 did not promote vector candidate visibly after proof");
  RequireEvidenceHygiene(result);

  request = BaseRequest();
  request.promotion_requested = true;
  request.stats_epoch = 43;
  result = api::EnginePlanNoSqlStatisticsAdvisor(request);
  Require(!result.ok, "ODF-078 stale stats did not fail closed");
  Require(DiagnosticContains(result, "SB_NOSQL_STATS_ADVISOR.STATS_STALE"),
          "ODF-078 stale stats diagnostic changed");

  request = BaseRequest();
  request.promotion_requested = true;
  request.security_redaction_proof_present = false;
  result = api::EnginePlanNoSqlStatisticsAdvisor(request);
  Require(!result.ok, "ODF-078 missing security proof did not fail closed");
  Require(DiagnosticContains(result,
                             "SB_NOSQL_STATS_ADVISOR.MISSING_SECURITY_PROOF"),
          "ODF-078 missing security proof diagnostic changed");

  request = BaseRequest();
  request.promotion_requested = true;
  request.engine_mga_authoritative = false;
  result = api::EnginePlanNoSqlStatisticsAdvisor(request);
  Require(!result.ok, "ODF-078 missing MGA authority did not fail closed");
  Require(DiagnosticContains(result,
                             "SB_NOSQL_STATS_ADVISOR.MISSING_RECHECK_PROOF"),
          "ODF-078 missing MGA recheck diagnostic changed");
}

void InsufficientBenefitRemainsAdvisoryAndInvisible() {
  auto request = BaseRequest();
  request.promotion_requested = true;
  request.candidate_benefit_score = 25;
  request.promotion_benefit_threshold = 100;
  const auto result = api::EnginePlanNoSqlStatisticsAdvisor(request);
  Require(result.ok, "ODF-078 insufficient benefit should remain advisory");
  Require(!result.advisor_result.promotion_succeeded,
          "ODF-078 insufficient benefit promoted a candidate");
  Require(result.advisor_result.candidate_invisible,
          "ODF-078 insufficient benefit did not remain invisible");
  Require(DiagnosticContains(result,
                             "SB_NOSQL_STATS_ADVISOR.INSUFFICIENT_BENEFIT"),
          "ODF-078 insufficient benefit diagnostic changed");
  Require(RowMatches(result,
                     {{"row_kind", "adaptive_index_candidate"},
                      {"family", "graph"},
                      {"promotion_state", "invisible"},
                      {"invisible", "true"},
                      {"promoted_visible", "false"}}),
          "ODF-078 graph candidate was not advisory invisible");
  RequireEvidenceHygiene(result);
}

void NonAuthoritativeParserReferenceAndFallbackAuthorityFailClosed() {
  auto request = BaseRequest();
  request.statistics.front().authoritative = false;
  auto result = api::EnginePlanNoSqlStatisticsAdvisor(request);
  Require(!result.ok,
          "ODF-078 non-authoritative stat row did not fail closed");
  Require(DiagnosticContains(result,
                             "SB_NOSQL_STATS_ADVISOR.STAT_NOT_AUTHORITATIVE"),
          "ODF-078 non-authoritative stat diagnostic changed");

  request = BaseRequest();
  request.parser_or_reference_authority = true;
  result = api::EnginePlanNoSqlStatisticsAdvisor(request);
  Require(!result.ok,
          "ODF-078 parser/reference authority did not fail closed");
  Require(DiagnosticContains(result, "SB_NOSQL_STATS_ADVISOR.UNSAFE_AUTHORITY"),
          "ODF-078 parser/reference authority diagnostic changed");

  request = BaseRequest();
  request.statistics.front().descriptor_scan_selected = true;
  result = api::EnginePlanNoSqlStatisticsAdvisor(request);
  Require(!result.ok,
          "ODF-078 descriptor scan stats authority did not fail closed");
  Require(DiagnosticContains(result,
                             "SB_NOSQL_STATS_ADVISOR.NON_PHYSICAL_STATS_SOURCE"),
          "ODF-078 descriptor scan diagnostic changed");

  request = BaseRequest();
  request.stats_catalog_authoritative = false;
  result = api::EnginePlanNoSqlStatisticsAdvisor(request);
  Require(!result.ok,
          "ODF-078 missing authoritative stats catalog did not fail closed");
  Require(DiagnosticContains(result,
                             "SB_NOSQL_STATS_ADVISOR.STATS_NOT_AUTHORITATIVE"),
          "ODF-078 stats catalog authority diagnostic changed");
}

void SecurityAndClusterGatesFailClosedAtApiBoundary() {
  auto request = BaseRequest();
  request.context.security_context_present = false;
  auto result = api::EnginePlanNoSqlStatisticsAdvisor(request);
  Require(!result.ok,
          "ODF-078 missing security context did not fail closed");
  Require(DiagnosticContains(result, "SB_ENGINE_API_SECURITY_CONTEXT_REQUIRED"),
          "ODF-078 security context diagnostic changed");

  request = BaseRequest();
  request.option_envelopes.push_back("cluster.route=required");
  result = api::EnginePlanNoSqlStatisticsAdvisor(request);
  Require(!result.ok && result.cluster_authority_required,
          "ODF-078 cluster-scoped statistics advisor did not require cluster authority");
  Require(DiagnosticContains(result, "SB_ENGINE_API_CLUSTER_AUTHORITY_UNAVAILABLE"),
          "ODF-078 cluster authority diagnostic changed");
}

}  // namespace

int main() {
  AllSixFamiliesEmitFamilySpecificStatsAndInvisibleCandidates();
  PromotionSucceedsOnlyAfterRequiredProof();
  InsufficientBenefitRemainsAdvisoryAndInvisible();
  NonAuthoritativeParserReferenceAndFallbackAuthorityFailClosed();
  SecurityAndClusterGatesFailClosedAtApiBoundary();
  return EXIT_SUCCESS;
}
