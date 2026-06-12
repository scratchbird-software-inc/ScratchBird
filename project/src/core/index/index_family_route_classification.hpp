// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// CEIC_038_LOSSY_PRUNING_FAMILY_CLASSIFICATION

#include "index_route_capability.hpp"

#include <string>
#include <vector>

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;

enum class IndexFamilyRouteSemantic : u32 {
  exact_candidate = 1,
  hash_equality_candidate = 2,
  bitmap_candidate = 3,
  token_candidate = 4,
  token_ranking_candidate = 5,
  spatial_candidate = 6,
  vector_exact_candidate = 7,
  vector_approximate_candidate = 8,
  document_path_candidate = 9,
  graph_seed_candidate = 10,
  bloom_negative_prune = 11,
  summary_segment_prune = 12,
  temporary_work_candidate = 13,
  in_memory_candidate = 14,
  reference_emulated_non_runtime = 15,
  policy_blocked_non_runtime = 16,
  unsupported = 17
};

enum class IndexRouteClassificationStatus : u32 {
  classified = 1,
  unsupported_family = 2,
  unsupported_route = 3,
  reference_emulated_non_runtime = 4,
  policy_blocked_non_runtime = 5,
  route_not_supported = 6,
  forbidden_authority_claim = 7,
  cluster_external_provider_only = 8,
  successor_or_enterprise_overclaim = 9
};

struct IndexRouteClassificationAuthorityClaims {
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool authorization_security_authority = false;
  bool security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool reference_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool optimizer_plan_finality_authority = false;
  bool index_finality_authority = false;
  bool row_truth_authority = false;
  bool final_row_authority = false;
  bool result_finality_authority = false;
  bool local_cluster_authority = false;
  bool cluster_action_authority = false;
  bool agent_action_authority = false;
};

struct IndexRouteClassificationSuccessorClaims {
  bool ceic_039_specialized_provider_closure_claimed = false;
  bool ceic_040_runtime_metrics_claimed = false;
  bool ceic_041_crash_matrix_claimed = false;
  bool ceic_042_readiness_drift_claimed = false;
  bool all_index_readiness_claimed = false;
  bool reference_dominance_claimed = false;
  bool enterprise_readiness_claimed = false;
};

struct IndexRouteClassificationRequest {
  IndexFamily family = IndexFamily::unknown;
  IndexRouteKind route = IndexRouteKind::unknown;
  IndexRouteClassificationAuthorityClaims authority_claims;
  IndexRouteClassificationSuccessorClaims successor_claims;
  bool cluster_path_requested = false;
  bool external_cluster_provider_only = true;
};

struct IndexRouteClassificationRequirements {
  bool exact_source_required = true;
  bool mga_visibility_recheck_required = true;
  bool security_recheck_required = true;
  bool authorization_recheck_required = true;
  bool predicate_recheck_required = true;
  bool ceic_037_exact_recheck_handoff_required = true;
  bool exact_fallback_required = false;
  bool exact_rerank_required = false;
  bool false_positive_accounting_required = false;
  bool stale_generation_policy_required = false;
  bool persistent_provider_closure_required = false;
  bool runtime_metrics_future_proof_required = true;
  bool crash_matrix_future_proof_required = true;
};

struct IndexRouteFamilyClassificationResult {
  Status status;
  bool classified = false;
  bool fail_closed = true;
  bool runtime_admissible = false;
  bool engine_owned_classification = true;
  bool ceic_038_family_classification_evidence = true;
  IndexRouteClassificationStatus classification_status =
      IndexRouteClassificationStatus::unsupported_family;
  IndexFamilyRouteSemantic semantic = IndexFamilyRouteSemantic::unsupported;
  IndexRouteClassificationRequirements requirements;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;

  bool produces_exact_candidates = false;
  bool produces_lossy_candidates = false;
  bool produces_candidate_set = false;
  bool produces_ranking = false;
  bool produces_seed_set = false;
  bool supports_negative_prune = false;
  bool negative_prune_only = false;
  bool supports_summary_segment_prune = false;
  bool summary_segment_prune_only = false;
  bool approximate_candidate_source = false;
  bool bitmap_candidate_source = false;
  bool token_or_inverted_candidate_source = false;
  bool vector_candidate_source = false;
  bool document_candidate_source = false;
  bool graph_candidate_source = false;
  bool spatial_candidate_source = false;
  bool hash_equality_only = false;
  bool supports_ordered_range = false;
  bool row_truth_authority = false;
  bool final_row_authority = false;

  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool authorization_authority = false;
  bool authorization_security_authority = false;
  bool security_authority = false;
  bool recovery_authority = false;
  bool parser_authority = false;
  bool reference_authority = false;
  bool wal_authority = false;
  bool benchmark_authority = false;
  bool optimizer_plan_authority = false;
  bool optimizer_plan_finality_authority = false;
  bool index_finality_authority = false;
  bool result_finality_authority = false;
  bool local_cluster_authority = false;
  bool cluster_action_authority = false;
  bool agent_action_authority = false;

  bool ceic_039_specialized_provider_closure_claimed = false;
  bool ceic_040_runtime_metrics_claimed = false;
  bool ceic_041_crash_matrix_claimed = false;
  bool ceic_042_readiness_drift_claimed = false;
  bool all_index_readiness_claimed = false;
  bool reference_dominance_claimed = false;
  bool enterprise_readiness_claimed = false;

  bool ok() const { return status.ok() && classified && !fail_closed; }
};

const char* IndexFamilyRouteSemanticName(IndexFamilyRouteSemantic semantic);
const char* IndexRouteClassificationStatusName(
    IndexRouteClassificationStatus status);
bool IndexRouteClassificationAuthorityClaimsClear(
    const IndexRouteClassificationAuthorityClaims& claims);
bool IndexRouteClassificationSuccessorClaimsClear(
    const IndexRouteClassificationSuccessorClaims& claims);
IndexRouteFamilyClassificationResult ClassifyIndexFamilyRoute(
    const IndexRouteClassificationRequest& request);

}  // namespace scratchbird::core::index
