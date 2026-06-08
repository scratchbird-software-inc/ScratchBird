// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "nosql/nosql_statistics_api.hpp"

#include "behavior_support/api_behavior_store.hpp"
#include "nosql/nosql_surface_support.hpp"

#include <string>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

namespace opt = scratchbird::engine::optimizer;

opt::NoSqlFamilyStatisticInput ToOptimizerStatistic(
    const EngineNoSqlStatisticInput& input) {
  opt::NoSqlFamilyStatisticInput stat;
  stat.family = input.family;
  stat.statistic_kind = input.statistic_kind;
  stat.statistic_key = input.statistic_key;
  stat.count = input.count;
  stat.distinct_values = input.distinct_values;
  stat.bucket_count = input.bucket_count;
  stat.vector_dimension = input.vector_dimension;
  stat.fresh = input.fresh;
  stat.authoritative = input.authoritative;
  stat.physical_provider_backed = input.physical_provider_backed;
  stat.descriptor_scan_selected = input.descriptor_scan_selected;
  stat.behavior_store_scan_selected = input.behavior_store_scan_selected;
  return stat;
}

opt::NoSqlStatisticsAdvisorRequest ToOptimizerRequest(
    const EnginePlanNoSqlStatisticsAdvisorRequest& request) {
  opt::NoSqlStatisticsAdvisorRequest advisor_request;
  advisor_request.object_uuid = request.target_object.uuid.canonical;
  if (advisor_request.object_uuid.empty()) {
    advisor_request.object_uuid = request.bound_object_identity.object_uuid.canonical;
  }
  advisor_request.stats_epoch = request.stats_epoch;
  advisor_request.required_stats_epoch = request.required_stats_epoch;
  advisor_request.catalog_epoch = request.catalog_epoch;
  advisor_request.security_epoch = request.security_epoch;
  advisor_request.policy_epoch = request.policy_epoch;
  advisor_request.stats_visibility_epoch = request.stats_visibility_epoch;
  advisor_request.stats_catalog_authoritative =
      request.stats_catalog_authoritative;
  advisor_request.stats_are_fresh = request.stats_are_fresh;
  advisor_request.security_redaction_proof_present =
      request.security_redaction_proof_present;
  advisor_request.security_snapshot_bound = request.security_snapshot_bound;
  advisor_request.grants_proven = request.grants_proven;
  advisor_request.engine_mga_authoritative = request.engine_mga_authoritative;
  advisor_request.mga_recheck_required = request.mga_recheck_required;
  advisor_request.security_recheck_required = request.security_recheck_required;
  advisor_request.parser_or_donor_authority =
      request.parser_or_donor_authority;
  advisor_request.provider_claims_transaction_finality_authority =
      request.provider_claims_transaction_finality_authority;
  advisor_request.provider_claims_visibility_authority =
      request.provider_claims_visibility_authority;
  advisor_request.client_claims_visibility_or_finality_authority =
      request.client_claims_visibility_or_finality_authority;
  advisor_request.write_ahead_log_claims_finality_authority =  // wal-not-authority
      request.write_ahead_log_claims_finality_authority;  // wal-not-authority
  advisor_request.promotion_requested = request.promotion_requested;
  advisor_request.candidate_benefit_score = request.candidate_benefit_score;
  advisor_request.promotion_benefit_threshold =
      request.promotion_benefit_threshold;
  for (const auto& statistic : request.statistics) {
    advisor_request.statistics.push_back(ToOptimizerStatistic(statistic));
  }
  return advisor_request;
}

void AddDiagnostic(EnginePlanNoSqlStatisticsAdvisorResult* result) {
  if (result->advisor_result.diagnostic_code.empty()) {
    return;
  }
  EngineApiDiagnostic diagnostic;
  diagnostic.code = result->advisor_result.diagnostic_code;
  diagnostic.message_key = "nosql.statistics_advisor";
  diagnostic.detail = result->advisor_result.evidence.empty()
                          ? result->advisor_result.diagnostic_code
                          : result->advisor_result.evidence.front();
  diagnostic.error = result->advisor_result.fail_closed;
  result->diagnostics.push_back(std::move(diagnostic));
}

void AddEvidence(EnginePlanNoSqlStatisticsAdvisorResult* result) {
  AddEngineNoSqlSurfaceEvidence(
      result,
      "statistics_advisor",
      "family_statistics_and_invisible_index_promotion_plan");
  AddApiBehaviorEvidence(result, "behavior_store_scan_selected", "false");
  AddApiBehaviorEvidence(result, "descriptor_scan_selected", "false");
  AddApiBehaviorEvidence(result,
                         "provider_transaction_finality_authority",
                         "false");
  AddApiBehaviorEvidence(result, "provider_visibility_authority", "false");
  AddApiBehaviorEvidence(result,
                         "parser_transaction_finality_authority",
                         "false");
  AddApiBehaviorEvidence(result, "client_autocommit_authority", "false");
  AddApiBehaviorEvidence(result, "row_mga_recheck_evidence", "required");
  AddApiBehaviorEvidence(result, "row_security_recheck_evidence", "required");
  for (const auto& field : result->advisor_result.evidence) {
    AddApiBehaviorEvidence(result, "nosql_statistics_advisor", field);
  }
}

void AddRows(EnginePlanNoSqlStatisticsAdvisorResult* result) {
  for (const auto& row : result->advisor_result.statistics_rows) {
    AddApiBehaviorRow(
        result,
        {{"row_kind", "family_statistic"},
         {"family", EngineNoSqlProviderFamilyName(row.family)},
         {"statistic_kind", row.statistic_kind},
         {"statistic_key", row.statistic_key},
         {"count", std::to_string(row.count)},
         {"distinct_values", std::to_string(row.distinct_values)},
         {"bucket_count", std::to_string(row.bucket_count)},
         {"vector_dimension", std::to_string(row.vector_dimension)}});
  }
  for (const auto& candidate : result->advisor_result.candidates) {
    AddApiBehaviorRow(
        result,
        {{"row_kind", "adaptive_index_candidate"},
         {"family", EngineNoSqlProviderFamilyName(candidate.family)},
         {"candidate_index_uuid", candidate.candidate_index_uuid},
         {"index_kind", candidate.index_kind},
         {"promotion_state", candidate.promotion_state},
         {"benefit_score", std::to_string(candidate.benefit_score)},
         {"benefit_threshold", std::to_string(candidate.benefit_threshold)},
         {"invisible", candidate.invisible ? "true" : "false"},
         {"promoted_visible", candidate.promoted_visible ? "true" : "false"}});
  }
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_STATISTICS_ADVISOR_API_BEHAVIOR
EnginePlanNoSqlStatisticsAdvisorResult EnginePlanNoSqlStatisticsAdvisor(
    const EnginePlanNoSqlStatisticsAdvisorRequest& request) {
  constexpr const char* kOperation = "nosql.statistics_advisor_plan";
  if (!request.context.security_context_present) {
    return MakeApiBehaviorDiagnostic<EnginePlanNoSqlStatisticsAdvisorResult>(
        request.context,
        kOperation,
        MakeSecurityContextRequiredDiagnostic(kOperation));
  }
  if (!request.context.cluster_authority_available &&
      EngineNoSqlRequiresClusterAuthority(request)) {
    return EngineNoSqlClusterAuthorityUnavailable<
        EnginePlanNoSqlStatisticsAdvisorResult>(request, kOperation);
  }

  EnginePlanNoSqlStatisticsAdvisorResult result;
  result.operation_id = kOperation;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.dml_summary.benchmark_clean = true;
  result.dml_summary.visible_rows_scanned = 0;
  result.advisor_result = opt::EvaluateNoSqlStatisticsAdvisor(
      ToOptimizerRequest(request));
  result.ok = result.advisor_result.ok;
  result.dml_summary.index_probes =
      static_cast<EngineApiU64>(result.advisor_result.candidates.size());
  AddEvidence(&result);
  AddRows(&result);
  AddDiagnostic(&result);
  return result;
}

}  // namespace scratchbird::engine::internal_api
