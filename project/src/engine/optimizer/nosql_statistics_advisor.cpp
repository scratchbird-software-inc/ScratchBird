// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql_statistics_advisor.hpp"

#include <algorithm>
#include <array>
#include <map>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::optimizer {
namespace {

namespace api = scratchbird::engine::internal_api;

using Family = api::EngineNoSqlProviderFamily;

void AddEvidence(NoSqlStatisticsAdvisorResult* result, std::string evidence) {
  if (result != nullptr) {
    result->evidence.push_back(std::move(evidence));
  }
}

NoSqlStatisticsAdvisorResult Refuse(std::string code, std::string evidence) {
  NoSqlStatisticsAdvisorResult result;
  result.ok = false;
  result.fail_closed = true;
  result.diagnostic_code = std::move(code);
  AddEvidence(&result, std::move(evidence));
  AddEvidence(&result, "statistics_metadata_only=true");
  AddEvidence(&result, "parser_or_donor_authority=false");
  AddEvidence(&result, "provider_transaction_finality_authority=false");
  AddEvidence(&result, "provider_visibility_authority=false");
  AddEvidence(&result, "client_visibility_or_finality_authority=false");
  AddEvidence(&result, "write_ahead_log_finality_authority=false");
  return result;
}

bool IsCoveredFamily(Family family) {
  return family == Family::kKeyValue ||
         family == Family::kDocument ||
         family == Family::kSearch ||
         family == Family::kVector ||
         family == Family::kGraph ||
         family == Family::kTimeSeries;
}

std::array<Family, 6> CoveredFamilies() {
  return {Family::kKeyValue,
          Family::kDocument,
          Family::kSearch,
          Family::kVector,
          Family::kGraph,
          Family::kTimeSeries};
}

std::vector<std::string_view> RequiredStatistics(Family family) {
  switch (family) {
    case Family::kKeyValue:
      return {"key_cardinality",
              "prefix_frequency",
              "ttl_active_count",
              "ttl_expired_count"};
    case Family::kDocument:
      return {"path_cardinality",
              "wildcard_path_frequency",
              "shape_dictionary_frequency",
              "shape_dictionary_ndv"};
    case Family::kSearch:
      return {"token_frequency",
              "document_frequency",
              "segment_count",
              "term_distribution"};
    case Family::kVector:
      return {"vector_count",
              "dimension_distribution",
              "metadata_filter_frequency",
              "sparse_term_frequency"};
    case Family::kGraph:
      return {"vertex_cardinality",
              "edge_cardinality",
              "label_frequency",
              "property_frequency",
              "degree_distribution",
              "frontier_distribution"};
    case Family::kTimeSeries:
      return {"metric_cardinality",
              "bucket_count",
              "meta_key_frequency",
              "rollup_distribution"};
    case Family::kSpatial:
    case Family::kColumnar:
    case Family::kUnknown:
      return {};
  }
  return {};
}

bool HasRequiredStats(const std::vector<NoSqlFamilyStatisticInput>& statistics,
                      Family family,
                      std::string* missing_kind) {
  for (const auto required : RequiredStatistics(family)) {
    const auto found = std::find_if(
        statistics.begin(),
        statistics.end(),
        [family, required](const NoSqlFamilyStatisticInput& stat) {
          return stat.family == family && stat.statistic_kind == required;
        });
    if (found == statistics.end()) {
      if (missing_kind != nullptr) {
        *missing_kind = std::string(api::EngineNoSqlProviderFamilyName(family)) +
                        "." + std::string(required);
      }
      return false;
    }
  }
  return true;
}

NoSqlFamilyStatisticRow RowFromInput(const NoSqlFamilyStatisticInput& input) {
  NoSqlFamilyStatisticRow row;
  row.family = input.family;
  row.statistic_kind = input.statistic_kind;
  row.statistic_key = input.statistic_key;
  row.count = input.count;
  row.distinct_values = input.distinct_values;
  row.bucket_count = input.bucket_count;
  row.vector_dimension = input.vector_dimension;
  return row;
}

void AddCommonAuthorityEvidence(NoSqlStatisticsAdvisorResult* result) {
  AddEvidence(result, "statistics_metadata_only=true");
  AddEvidence(result, "catalog_statistics_authority=authoritative_physical_stats");
  AddEvidence(result, "physical_stats_authority=family_provider_catalog");
  AddEvidence(result, "descriptor_scan_selected=false");
  AddEvidence(result, "behavior_store_scan_selected=false");
  AddEvidence(result, "mga_visibility_authority=engine_recheck_required");
  AddEvidence(result, "mga_finality_authority=engine_transaction_inventory");
  AddEvidence(result, "security_recheck=required");
  AddEvidence(result, "security_redaction_proof=present");
  AddEvidence(result, "parser_or_donor_authority=false");
  AddEvidence(result, "provider_transaction_finality_authority=false");
  AddEvidence(result, "provider_visibility_authority=false");
  AddEvidence(result, "client_visibility_or_finality_authority=false");
  AddEvidence(result, "write_ahead_log_finality_authority=false");
  AddEvidence(result, "result_semantics_changed=false");
}

void AddFamilyEvidence(NoSqlStatisticsAdvisorResult* result,
                       const std::map<Family, std::uint64_t>& counts) {
  for (const auto family : CoveredFamilies()) {
    const auto found = counts.find(family);
    AddEvidence(result,
                std::string("family_stats_rows.") +
                    api::EngineNoSqlProviderFamilyName(family) + "=" +
                    std::to_string(found == counts.end() ? 0 : found->second));
  }
}

NoSqlAdaptiveIndexCandidate BuildCandidate(
    Family family,
    const NoSqlStatisticsAdvisorRequest& request) {
  NoSqlAdaptiveIndexCandidate candidate;
  candidate.family = family;
  candidate.candidate_index_uuid =
      request.object_uuid + ":" + api::EngineNoSqlProviderFamilyName(family) +
      ":adaptive";
  candidate.index_kind = NoSqlAdaptiveIndexKindForFamily(family);
  candidate.benefit_score = request.candidate_benefit_score;
  candidate.benefit_threshold = request.promotion_benefit_threshold;
  candidate.invisible = true;
  candidate.promoted_visible = false;
  candidate.promotion_state = "invisible";
  return candidate;
}

}  // namespace

const char* NoSqlAdaptiveIndexKindForFamily(Family family) {
  switch (family) {
    case Family::kKeyValue:
      return "kv_prefix_ttl_adaptive_index";
    case Family::kDocument:
      return "document_path_shape_adaptive_index";
    case Family::kSearch:
      return "search_term_segment_adaptive_index";
    case Family::kVector:
      return "vector_metadata_sparse_adaptive_index";
    case Family::kGraph:
      return "graph_label_degree_adaptive_index";
    case Family::kTimeSeries:
      return "time_series_metric_bucket_adaptive_index";
    case Family::kSpatial:
    case Family::kColumnar:
    case Family::kUnknown:
      return "unknown_adaptive_index";
  }
  return "unknown_adaptive_index";
}

NoSqlStatisticsAdvisorResult EvaluateNoSqlStatisticsAdvisor(
    const NoSqlStatisticsAdvisorRequest& request) {
  if (request.object_uuid.empty()) {
    return Refuse("SB_NOSQL_STATS_ADVISOR.OBJECT_REQUIRED",
                  "object_required");
  }
  if (request.parser_or_donor_authority ||
      request.provider_claims_transaction_finality_authority ||
      request.provider_claims_visibility_authority ||
      request.client_claims_visibility_or_finality_authority ||
      request.write_ahead_log_claims_finality_authority) {
    return Refuse("SB_NOSQL_STATS_ADVISOR.UNSAFE_AUTHORITY",
                  "unsafe_authority_claim_refused");
  }
  if (!request.stats_catalog_authoritative) {
    return Refuse("SB_NOSQL_STATS_ADVISOR.STATS_NOT_AUTHORITATIVE",
                  "stats_catalog_not_authoritative");
  }
  if (!request.stats_are_fresh ||
      request.stats_epoch == 0 ||
      request.required_stats_epoch == 0 ||
      request.stats_epoch < request.required_stats_epoch) {
    return Refuse("SB_NOSQL_STATS_ADVISOR.STATS_STALE",
                  "stats_stale_or_missing_epoch");
  }
  if (request.catalog_epoch == 0 ||
      request.security_epoch == 0 ||
      request.policy_epoch == 0 ||
      request.stats_visibility_epoch == 0) {
    return Refuse("SB_NOSQL_STATS_ADVISOR.MISSING_EPOCH_EVIDENCE",
                  "missing_epoch_evidence");
  }
  if (!request.security_redaction_proof_present ||
      !request.security_snapshot_bound ||
      !request.grants_proven) {
    return Refuse("SB_NOSQL_STATS_ADVISOR.MISSING_SECURITY_PROOF",
                  "missing_security_redaction_or_grants_proof");
  }
  if (!request.engine_mga_authoritative ||
      !request.mga_recheck_required ||
      !request.security_recheck_required) {
    return Refuse("SB_NOSQL_STATS_ADVISOR.MISSING_RECHECK_PROOF",
                  "missing_mga_or_security_recheck_proof");
  }
  if (request.statistics.empty()) {
    return Refuse("SB_NOSQL_STATS_ADVISOR.STATS_MISSING",
                  "stats_missing");
  }

  std::map<Family, std::uint64_t> row_counts;
  for (const auto& stat : request.statistics) {
    if (!IsCoveredFamily(stat.family)) {
      return Refuse("SB_NOSQL_STATS_ADVISOR.UNSUPPORTED_FAMILY",
                    "unsupported_family");
    }
    if (!stat.authoritative || !stat.physical_provider_backed) {
      return Refuse("SB_NOSQL_STATS_ADVISOR.STAT_NOT_AUTHORITATIVE",
                    "stat_not_authoritative_or_not_physical");
    }
    if (!stat.fresh) {
      return Refuse("SB_NOSQL_STATS_ADVISOR.STATS_STALE",
                    "stat_row_stale");
    }
    if (stat.descriptor_scan_selected || stat.behavior_store_scan_selected) {
      return Refuse("SB_NOSQL_STATS_ADVISOR.NON_PHYSICAL_STATS_SOURCE",
                    "descriptor_or_behavior_scan_not_stats_authority");
    }
    ++row_counts[stat.family];
  }
  for (const auto family : CoveredFamilies()) {
    std::string missing_kind;
    if (!HasRequiredStats(request.statistics, family, &missing_kind)) {
      return Refuse("SB_NOSQL_STATS_ADVISOR.FAMILY_STATS_INCOMPLETE",
                    "missing_family_stat=" + missing_kind);
    }
  }

  NoSqlStatisticsAdvisorResult result;
  result.ok = true;
  result.fail_closed = false;
  result.statistics_catalog_planned = true;
  result.diagnostic_code = "SB_NOSQL_STATS_ADVISOR.CANDIDATE_INVISIBLE";

  for (const auto& input : request.statistics) {
    result.statistics_rows.push_back(RowFromInput(input));
  }

  AddCommonAuthorityEvidence(&result);
  AddFamilyEvidence(&result, row_counts);
  AddEvidence(&result, "stats_epoch_fresh=true");
  AddEvidence(&result, "catalog_epoch_compatible=true");
  AddEvidence(&result, "security_epoch_compatible=true");
  AddEvidence(&result, "policy_epoch_compatible=true");
  AddEvidence(&result, "stats_visibility_epoch_metadata_only=true");

  for (const auto family : CoveredFamilies()) {
    auto candidate = BuildCandidate(family, request);
    result.candidates.push_back(std::move(candidate));
  }
  result.candidate_built = !result.candidates.empty();
  result.candidate_invisible = result.candidate_built;
  AddEvidence(&result, "adaptive_index_candidates_built=invisible");

  if (request.candidate_benefit_score < request.promotion_benefit_threshold) {
    result.diagnostic_code = "SB_NOSQL_STATS_ADVISOR.INSUFFICIENT_BENEFIT";
    AddEvidence(&result, "promotion_state=advisory_invisible");
    AddEvidence(&result, "benefit_threshold_met=false");
    return result;
  }

  AddEvidence(&result, "benefit_threshold_met=true");
  if (!request.promotion_requested) {
    AddEvidence(&result, "promotion_state=advisory_invisible");
    return result;
  }

  for (auto& candidate : result.candidates) {
    candidate.invisible = false;
    candidate.promoted_visible = true;
    candidate.promotion_state = "visible_active";
  }
  result.candidate_invisible = false;
  result.promotion_succeeded = true;
  result.diagnostic_code = "SB_NOSQL_STATS_ADVISOR.PROMOTED_VISIBLE";
  AddEvidence(&result, "promotion_state=visible_active");
  AddEvidence(&result, "promotion_requires_fresh_stats=true");
  AddEvidence(&result, "promotion_requires_security_recheck=true");
  AddEvidence(&result, "promotion_requires_mga_recheck=true");
  return result;
}

std::string SerializeNoSqlStatisticsAdvisorEvidence(
    const NoSqlStatisticsAdvisorResult& result) {
  std::ostringstream out;
  out << "diagnostic_code=" << result.diagnostic_code
      << "|ok=" << (result.ok ? "true" : "false")
      << "|fail_closed=" << (result.fail_closed ? "true" : "false")
      << "|statistics_catalog_planned="
      << (result.statistics_catalog_planned ? "true" : "false")
      << "|candidate_built="
      << (result.candidate_built ? "true" : "false")
      << "|candidate_invisible="
      << (result.candidate_invisible ? "true" : "false")
      << "|promotion_succeeded="
      << (result.promotion_succeeded ? "true" : "false")
      << "|row_visibility_semantics_changed="
      << (result.row_visibility_semantics_changed ? "true" : "false")
      << "|transaction_finality_semantics_changed="
      << (result.transaction_finality_semantics_changed ? "true" : "false");
  for (const auto& evidence : result.evidence) {
    out << '|' << evidence;
  }
  return out.str();
}

}  // namespace scratchbird::engine::optimizer
