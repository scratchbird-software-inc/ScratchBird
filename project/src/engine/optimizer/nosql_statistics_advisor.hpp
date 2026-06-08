// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "nosql/nosql_physical_provider_contract.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_NOSQL_STATISTICS_ADVISOR_ODF_078
// Family statistics and adaptive index promotion are optimizer/catalog
// metadata. They do not own transaction finality, visibility, parser
// execution, provider authority, client authority, or recovery semantics.

struct NoSqlFamilyStatisticInput {
  scratchbird::engine::internal_api::EngineNoSqlProviderFamily family =
      scratchbird::engine::internal_api::EngineNoSqlProviderFamily::kUnknown;
  std::string statistic_kind;
  std::string statistic_key;
  std::uint64_t count = 0;
  std::uint64_t distinct_values = 0;
  std::uint64_t bucket_count = 0;
  std::uint64_t vector_dimension = 0;
  bool fresh = false;
  bool authoritative = false;
  bool physical_provider_backed = false;
  bool descriptor_scan_selected = false;
  bool behavior_store_scan_selected = false;
};

struct NoSqlFamilyStatisticRow {
  scratchbird::engine::internal_api::EngineNoSqlProviderFamily family =
      scratchbird::engine::internal_api::EngineNoSqlProviderFamily::kUnknown;
  std::string statistic_kind;
  std::string statistic_key;
  std::uint64_t count = 0;
  std::uint64_t distinct_values = 0;
  std::uint64_t bucket_count = 0;
  std::uint64_t vector_dimension = 0;
};

struct NoSqlAdaptiveIndexCandidate {
  scratchbird::engine::internal_api::EngineNoSqlProviderFamily family =
      scratchbird::engine::internal_api::EngineNoSqlProviderFamily::kUnknown;
  std::string candidate_index_uuid;
  std::string index_kind;
  std::string promotion_state = "invisible";
  std::uint64_t benefit_score = 0;
  std::uint64_t benefit_threshold = 0;
  bool invisible = true;
  bool promoted_visible = false;
};

struct NoSqlStatisticsAdvisorRequest {
  std::string object_uuid;
  std::uint64_t stats_epoch = 0;
  std::uint64_t required_stats_epoch = 0;
  std::uint64_t catalog_epoch = 0;
  std::uint64_t security_epoch = 0;
  std::uint64_t policy_epoch = 0;
  std::uint64_t stats_visibility_epoch = 0;
  std::vector<NoSqlFamilyStatisticInput> statistics;

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
  bool write_ahead_log_claims_finality_authority = false;

  bool promotion_requested = false;
  std::uint64_t candidate_benefit_score = 0;
  std::uint64_t promotion_benefit_threshold = 0;
};

struct NoSqlStatisticsAdvisorResult {
  bool ok = false;
  bool fail_closed = true;
  bool statistics_catalog_planned = false;
  bool candidate_built = false;
  bool candidate_invisible = false;
  bool promotion_succeeded = false;
  bool row_visibility_semantics_changed = false;
  bool transaction_finality_semantics_changed = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  std::vector<NoSqlFamilyStatisticRow> statistics_rows;
  std::vector<NoSqlAdaptiveIndexCandidate> candidates;
};

NoSqlStatisticsAdvisorResult EvaluateNoSqlStatisticsAdvisor(
    const NoSqlStatisticsAdvisorRequest& request);

const char* NoSqlAdaptiveIndexKindForFamily(
    scratchbird::engine::internal_api::EngineNoSqlProviderFamily family);

std::string SerializeNoSqlStatisticsAdvisorEvidence(
    const NoSqlStatisticsAdvisorResult& result);

}  // namespace scratchbird::engine::optimizer
