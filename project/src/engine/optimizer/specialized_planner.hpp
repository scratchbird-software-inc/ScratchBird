// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "access_path.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

// SEARCH_KEY: SB_OPTIMIZER_SPECIALIZED_FAMILY_PLANNER
struct SpecializedProviderCapability {
  std::string family;
  bool local_provider_available = false;
  bool index_available = false;
  bool exact_fallback_available = false;
  bool descriptor_compatible = false;
  bool policy_allowed = false;
  bool descriptor_visibility_proof_present = false;
  bool descriptor_visible_to_snapshot = false;
  bool security_redaction_proof_present = false;
  bool security_snapshot_proof_present = false;
  bool index_generation_proof_present = false;
  bool index_generation_visible_to_snapshot = false;
  bool index_covers_predicate = false;
  std::uint64_t required_index_generation = 0;
  std::uint64_t available_index_generation = 0;
  bool delta_overlay_required = false;
  bool delta_overlay_proof_present = false;
  bool delta_overlay_covers_snapshot = false;
  bool policy_proof_present = false;
  bool mga_recheck_proof_present = false;
  bool row_mga_recheck_required = true;
  bool row_security_recheck_required = true;
  bool requires_cluster_provider = false;
  bool requires_distributed_provider = false;
  bool descriptor_scan_path = false;
  bool behavior_store_scan_path = false;
  bool provider_claims_transaction_finality_authority = false;
  bool provider_claims_visibility_authority = false;
  bool index_claims_transaction_finality_authority = false;
  bool delta_overlay_claims_transaction_finality_authority = false;
  bool parser_claims_transaction_finality_authority = false;
  bool write_ahead_log_claims_transaction_finality_authority = false;
  std::string provider_id = "nosql.local.provider";
  std::string fallback_provider_id;
  std::uint64_t estimated_rows = 1000;
};

// SEARCH_KEY: OPCH_SPECIALIZED_WORKLOAD_FAMILY_COVERAGE
struct SpecializedWorkloadFamilyCoverage {
  std::string family;
  std::string index_family;
  bool production_claim_removed = false;
  bool cost_statistics_present = false;
  bool selectivity_statistics_present = false;
  bool false_positive_statistics_present = false;
  bool route_gate_present = false;
  bool exact_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  double false_positive_ratio = 0.0;
};

struct SpecializedWorkloadFamilyCoverageResult {
  bool ok = false;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence;
};

// SEARCH_KEY: OPCH_NOSQL_SQL_FUSION_OPTIMIZER_ROUTES
struct NoSqlSqlFusionRouteRequest {
  std::string route_label;
  std::string sql_result_hash;
  std::string fusion_result_hash;
  bool sql_route_consumed = false;
  bool document_route_consumed = false;
  bool vector_route_consumed = false;
  bool search_route_consumed = false;
  bool graph_route_consumed = false;
  bool candidate_set_route_consumed = false;
  bool exact_recheck_required = true;
  bool mga_recheck_required = true;
  bool security_recheck_required = true;
  bool descriptor_scan_fallback = false;
  bool behavior_store_scan_fallback = false;
  bool parser_or_reference_authority = false;
  std::vector<PlanCandidate> candidates;
};

struct NoSqlSqlFusionRouteResult {
  bool ok = false;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

SpecializedProviderCapability MakeSpecializedProviderCapabilityFromContract(
    const scratchbird::engine::internal_api::EngineNoSqlPhysicalProviderContract& contract);
scratchbird::engine::internal_api::EngineNoSqlPhysicalProviderContract MakeNoSqlPhysicalProviderContract(
    const SpecializedProviderCapability& capability);
PlanCandidate PlanNoSqlPhysicalProviderCandidate(const SpecializedProviderCapability& capability);
PlanCandidate PlanNoSqlPhysicalProviderCandidate(
    const scratchbird::engine::internal_api::EngineNoSqlPhysicalProviderContract& contract);
PlanCandidate PlanNoSqlLogicalNodeCandidate(
    const scratchbird::engine::planner::LogicalPlanNode& node,
    const OptimizerStatisticsCatalog& statistics);
PlanCandidate PlanDocumentKvCandidate(const SpecializedProviderCapability& capability);
PlanCandidate PlanSearchCandidate(const SpecializedProviderCapability& capability);
PlanCandidate PlanVectorCandidate(const SpecializedProviderCapability& capability);
PlanCandidate PlanSpatialCandidate(const SpecializedProviderCapability& capability);
PlanCandidate PlanGraphCandidate(const SpecializedProviderCapability& capability);
PlanCandidate PlanTimeSeriesCandidate(const SpecializedProviderCapability& capability);
PlanCandidate PlanColumnarCandidate(const SpecializedProviderCapability& capability);
std::vector<PlanCandidate> PlanAllSpecializedFamilyCandidates(const std::vector<SpecializedProviderCapability>& capabilities);
SpecializedWorkloadFamilyCoverageResult ValidateSpecializedWorkloadFamilyCoverage(
    const std::vector<SpecializedWorkloadFamilyCoverage>& families);
NoSqlSqlFusionRouteResult ValidateNoSqlSqlFusionRoute(
    const NoSqlSqlFusionRouteRequest& request);

}  // namespace scratchbird::engine::optimizer
