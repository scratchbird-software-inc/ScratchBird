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

// SEARCH_KEY: SB_OPTIMIZER_NOSQL_APPROX_FILTER_DECISION_ODF_080
// Approximate filters are candidate-pruning metadata only. They never own row
// visibility, authorization, transaction finality, parser execution, provider
// finality, client finality, or recovery authority.

enum class NoSqlApproxFilterKind {
  kUnknown,
  kMinMaxSet,
  kBloom,
  kRangeFilter,
};

struct NoSqlApproxFilterBenchmarkInput {
  scratchbird::engine::internal_api::EngineNoSqlProviderFamily family =
      scratchbird::engine::internal_api::EngineNoSqlProviderFamily::kUnknown;
  NoSqlApproxFilterKind kind = NoSqlApproxFilterKind::kUnknown;
  std::string candidate_id;

  std::uint64_t benchmark_epoch = 0;
  std::uint64_t required_benchmark_epoch = 0;
  std::uint64_t input_rows = 0;
  std::uint64_t candidate_rows = 0;
  std::uint64_t pruned_rows = 0;
  std::uint64_t baseline_cost_units = 0;
  std::uint64_t filter_cost_units = 0;
  std::uint64_t exact_fallback_cost_units = 0;
  std::uint64_t observed_false_positive_ppm = 0;
  std::uint64_t observed_false_negative_ppm = 0;
  std::uint64_t observed_authorization_leak_count = 0;

  std::string encoded_min;
  std::string encoded_max;
  std::string predicate_low;
  std::string predicate_high;

  bool benchmark_authoritative = false;
  bool physical_provider_backed = false;
  bool exact_fallback_available = false;
  bool candidate_only = true;
  bool false_positive_disclosed = true;
  bool range_summary_valid = true;
  bool minmax_bounds_valid = true;
  bool descriptor_scan_selected = false;
  bool behavior_store_scan_selected = false;
  bool candidate_claims_visibility_or_finality_authority = false;
};

struct NoSqlApproxFilterDecisionRequest {
  std::string object_uuid;
  std::vector<NoSqlApproxFilterBenchmarkInput> candidates;

  std::uint64_t min_net_benefit_units = 1;
  bool security_context_present = false;
  bool security_snapshot_bound = false;
  bool grants_proven = false;
  bool cluster_scope_requested = false;
  bool cluster_authority_present = false;
  bool engine_mga_authoritative = false;
  bool exact_fallback_available = false;
  bool row_mga_recheck_required = true;
  bool row_security_recheck_required = true;
  bool parser_or_donor_authority = false;
  bool provider_claims_transaction_finality_authority = false;
  bool provider_claims_visibility_authority = false;
  bool client_claims_visibility_or_finality_authority = false;
  bool write_ahead_log_claims_finality_authority = false;
};

struct NoSqlApproxFilterCandidateDecision {
  scratchbird::engine::internal_api::EngineNoSqlProviderFamily family =
      scratchbird::engine::internal_api::EngineNoSqlProviderFamily::kUnknown;
  NoSqlApproxFilterKind kind = NoSqlApproxFilterKind::kUnknown;
  std::string candidate_id;
  std::uint64_t net_benefit_units = 0;
  std::uint64_t observed_false_positive_ppm = 0;
  bool selected = false;
  bool safe = false;
  bool refused = false;
  bool exact_fallback_required = true;
  bool row_mga_recheck_required = true;
  bool row_security_recheck_required = true;
  bool returns_final_rows = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

struct NoSqlApproxSelectedFilter {
  scratchbird::engine::internal_api::EngineNoSqlProviderFamily family =
      scratchbird::engine::internal_api::EngineNoSqlProviderFamily::kUnknown;
  NoSqlApproxFilterKind kind = NoSqlApproxFilterKind::kUnknown;
  std::string candidate_id;
  std::uint64_t net_benefit_units = 0;
  bool exact_fallback_required = true;
  bool row_mga_recheck_required = true;
  bool row_security_recheck_required = true;
  bool candidate_only = true;
};

struct NoSqlApproxFilterDecisionResult {
  bool ok = false;
  bool fail_closed = true;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
  std::vector<NoSqlApproxFilterCandidateDecision> candidate_decisions;
  std::vector<NoSqlApproxSelectedFilter> selected_filters;
};

NoSqlApproxFilterDecisionResult EvaluateNoSqlApproxFilterDecision(
    const NoSqlApproxFilterDecisionRequest& request);

const char* NoSqlApproxFilterKindName(NoSqlApproxFilterKind kind);
std::vector<scratchbird::engine::internal_api::EngineNoSqlProviderFamily>
NoSqlApproxFilterCoveredFamilies();
std::vector<NoSqlApproxFilterKind> NoSqlApproxFilterCandidateKinds();

std::string SerializeNoSqlApproxFilterDecisionEvidence(
    const NoSqlApproxFilterDecisionResult& result);

}  // namespace scratchbird::engine::optimizer
