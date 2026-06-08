// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-INDEX-OPTIMIZER-INTEGRATION-CLOSURE-ANCHOR

#include "index_access_method.hpp"
#include "index_metrics.hpp"
#include "index_route_capability.hpp"
#include "page_extent_summary.hpp"

namespace scratchbird::core::index {

using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

inline constexpr const char* kPageExtentSummaryPrunePlannerSearchKey =
    "DPC_PAGE_EXTENT_SUMMARY_PRUNE_PLANNER";

enum class IndexPlanCategory : u32 {
  point_lookup = 1,
  range_scan = 2,
  bitmap_combine = 3,
  summary_prune = 4,
  inverted_search = 5,
  spatial_search = 6,
  vector_search = 7,
  graph_search = 8,
  fallback_full_scan = 9
};

struct IndexReadinessPlanAdmissionEvidence {
  IndexFamily family = IndexFamily::unknown;
  IndexRouteKind route = IndexRouteKind::unknown;
  u64 manifest_epoch = 0;
  u64 registry_epoch = 0;
  u64 route_proof_epoch = 0;
  std::string source_evidence_digest;
  std::string generated_by;
  bool generated_manifest_present = false;
  bool generated_manifest_current = false;
  bool generated_manifest_validated = false;
  bool source_digest_matches = false;
  bool static_registry_only = false;
  bool smoke_only = false;
  bool drifted = false;
  bool placeholder_epoch = false;
  bool local_default_evidence = false;
  bool policy_default_evidence = false;
  bool synthetic_evidence = false;
  bool test_fixture_evidence = false;
  bool donor_emulated = false;
  bool policy_blocked = false;
  bool contract_only_family = false;
  bool runtime_registry_family_matches = false;
  bool runtime_registry_route_matches = false;
  bool runtime_family_available = false;
  bool runtime_route_complete = false;
  bool supports_read = false;
  bool supports_equality_lookup = false;
  bool supports_ordered_range = false;
  bool supports_negative_prune = false;
  bool supports_summary_segment_prune = false;
  bool produces_candidate_set = false;
  bool approximate_candidate_source = false;
  bool requires_exact_recheck = true;
  bool requires_mga_recheck = true;
  bool requires_security_recheck = true;
  bool requires_exact_rerank = false;
  bool exact_recheck_proven = false;
  bool mga_recheck_proven = false;
  bool security_recheck_proven = false;
  bool exact_rerank_proven = false;
  bool operation_metrics_producer_proven = false;
  bool support_bundle_producer_proven = false;
  bool crash_reopen_proven = false;
  bool corruption_cleanup_proven = false;
  bool cleanup_horizon_proven = false;
  bool storage_integration_proven = false;
  bool external_cluster_provider_only = true;
  bool local_cluster_authority = false;
  bool external_cluster_runtime_overclaim = false;
  bool parser_authority = false;
  bool client_authority = false;
  bool donor_authority = false;
  bool wal_authority = false;
  bool recovery_authority = false;
  bool transaction_finality_authority = false;
  bool visibility_authority = false;
  bool security_authority = false;
  bool provider_finality_authority = false;
  bool index_finality_authority = false;
  bool optimizer_plan_authority = false;
  bool agent_authority = false;
};

struct PageExtentSummaryPrunePredicate {
  std::string scalar_type_key;
  std::string encoded_lower;
  std::string encoded_upper;
  bool lower_present = false;
  bool upper_present = false;
  bool lower_inclusive = true;
  bool upper_inclusive = true;
};

struct PageExtentSummaryPruneCounters {
  u64 candidate_ranges = 0;
  u64 ranges_pruned = 0;
  u64 ranges_scanned = 0;
  u64 pages_considered = 0;
  u64 pages_pruned = 0;
  u64 pages_scanned = 0;
};

struct PageExtentSummaryPruneRequest {
  std::vector<PageExtentSummaryMetadata> summaries;
  PageExtentSummaryFormatCompatibility format;
  PageExtentSummaryPrunePredicate predicate;
  bool summary_prune_enabled = true;
  bool base_row_mga_recheck_required = true;
  bool base_row_security_recheck_required = true;
};

struct PageExtentSummaryPrunePlan {
  Status status;
  DiagnosticRecord diagnostic;
  IndexPlanCategory selected_category = IndexPlanCategory::fallback_full_scan;
  std::string selected_access = "full_scan";
  std::string prune_reason = "none";
  std::string fallback_reason = "none";
  std::string summary_status = "missing";
  u64 summary_generation = 0;
  std::string authority_source = "engine_mga_base_pages";
  PageExtentSummaryPruneCounters counters;
  bool summary_prune_selected = false;
  bool full_scan_fallback = true;
  bool base_row_mga_recheck_required = true;
  bool base_row_security_recheck_required = true;
  bool summary_metadata_visibility_authority = false;
  bool summary_metadata_finality_authority = false;
  std::vector<std::string> actions;

  bool ok() const { return status.ok() && summary_prune_selected; }
};

struct IndexOptimizerRequest {
  TypedUuid index_uuid;
  IndexFamily family = IndexFamily::unknown;
  IndexRouteKind route = IndexRouteKind::sql_select;
  IndexPlanCategory category = IndexPlanCategory::point_lookup;
  const IndexReadinessPlanAdmissionEvidence* readiness_evidence = nullptr;
  double selectivity = 1.0;
  double confidence = 0.0;
  bool stats_available = false;
  bool stats_stale = false;
  bool requires_equality_lookup = false;
  bool requires_range_scan = false;
  bool requires_negative_prune = false;
  bool requires_summary_segment_prune = false;
  bool requires_candidate_set = false;
  bool requires_exact_rows = true;
  bool requires_order = false;
  bool order_proven = false;
  bool covering_requested = false;
  bool covering_payload_fresh = false;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool approximate = false;
  bool exact_rerank_available = false;
  bool multi_index_requested = false;
  bool hash_keyed_algorithm_active = true;
  bool hash_legacy_algorithm_allowed_by_policy = false;
  bool hash_high_assurance_required = false;
  bool hash_high_assurance_active = false;
  u32 multi_index_inputs = 0;
};

struct IndexOptimizerPlan {
  Status status;
  IndexRouteKind route = IndexRouteKind::unknown;
  bool admitted = false;
  bool fallback_full_scan = false;
  bool fallback_sort = false;
  bool exact_recheck = true;
  bool rerank = false;
  bool index_only = false;
  bool multi_index_allowed = false;
  bool route_benchmark_clean = false;
  std::string route_capability = "unknown";
  double cost_multiplier = 1.0;
  std::vector<std::string> steps;
  DiagnosticRecord diagnostic;

  bool ok() const { return status.ok() && admitted; }
};

PageExtentSummaryPrunePlan PlanPageExtentSummaryPrune(
    const PageExtentSummaryPruneRequest& request);
IndexOptimizerPlan PlanIndexOptimizerPath(const IndexOptimizerRequest& request);
IndexOptimizerPlan PlanIndexExecutorDispatch(const IndexOptimizerRequest& request);
DiagnosticRecord MakeIndexOptimizerIntegrationDiagnostic(Status status,
                                                         std::string diagnostic_code,
                                                         std::string message_key,
                                                         std::string detail = {});

}  // namespace scratchbird::core::index
