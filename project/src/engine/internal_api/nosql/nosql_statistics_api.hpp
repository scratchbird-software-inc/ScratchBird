// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"
#include "nosql_statistics_advisor.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

struct EngineNoSqlStatisticInput {
  EngineNoSqlProviderFamily family = EngineNoSqlProviderFamily::kUnknown;
  std::string statistic_kind;
  std::string statistic_key;
  EngineApiU64 count = 0;
  EngineApiU64 distinct_values = 0;
  EngineApiU64 bucket_count = 0;
  EngineApiU64 vector_dimension = 0;
  bool fresh = false;
  bool authoritative = false;
  bool physical_provider_backed = false;
  bool descriptor_scan_selected = false;
  bool behavior_store_scan_selected = false;
};

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_NOSQL_STATISTICS_ADVISOR_API
struct EnginePlanNoSqlStatisticsAdvisorRequest : EngineApiRequest {
  std::vector<EngineNoSqlStatisticInput> statistics;
  EngineApiU64 stats_epoch = 0;
  EngineApiU64 required_stats_epoch = 0;
  EngineApiU64 catalog_epoch = 0;
  EngineApiU64 security_epoch = 0;
  EngineApiU64 policy_epoch = 0;
  EngineApiU64 stats_visibility_epoch = 0;

  bool stats_catalog_authoritative = false;
  bool stats_are_fresh = false;
  bool security_redaction_proof_present = false;
  bool security_snapshot_bound = false;
  bool grants_proven = false;
  bool engine_mga_authoritative = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool parser_or_donor_authority = false;
  bool provider_claims_transaction_finality_authority = false;
  bool provider_claims_visibility_authority = false;
  bool client_claims_visibility_or_finality_authority = false;
  bool write_ahead_log_claims_finality_authority = false;  // wal-not-authority

  bool promotion_requested = false;
  EngineApiU64 candidate_benefit_score = 0;
  EngineApiU64 promotion_benefit_threshold = 0;
};

struct EnginePlanNoSqlStatisticsAdvisorResult : EngineApiResult {
  scratchbird::engine::optimizer::NoSqlStatisticsAdvisorResult advisor_result;
};

EnginePlanNoSqlStatisticsAdvisorResult EnginePlanNoSqlStatisticsAdvisor(
    const EnginePlanNoSqlStatisticsAdvisorRequest& request);

}  // namespace scratchbird::engine::internal_api
