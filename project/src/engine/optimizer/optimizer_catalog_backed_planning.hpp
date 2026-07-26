// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_contract.hpp"
#ifndef SCRATCHBIRD_QOW_CANONICAL_CANDIDATE_LEGALITY_ONLY
#include "optimizer_plan_cache.hpp"
#include "optimizer_request.hpp"
#endif

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

enum class CanonicalOptimizerAdmissionStage : std::uint8_t {
  kBoundRequest = 1,
  kCatalogEpoch,
  kSecurity,
  kMgaStatementBoundary,
  kPolicyCapability,
  kResource,
  kStatisticsProvenance,
  kCanonicalRoute,
};

struct CanonicalOptimizerCatalogSnapshot {
  std::string snapshot_uuid;
  std::string catalog_epoch_uuid;
  std::uint64_t catalog_generation{0};
  std::vector<std::string> object_uuids;
  std::vector<std::uint32_t> descriptor_ids;
  bool engine_owned{false};
};

struct CanonicalOptimizerSecuritySnapshot {
  std::string security_context_uuid;
  std::uint64_t security_epoch{0};
  std::uint64_t policy_epoch{0};
  std::uint64_t catalog_generation{0};
  std::vector<std::string> authorized_object_uuids;
  bool engine_owned{false};
};

struct CanonicalOptimizerMgaSnapshot {
  std::uint64_t local_transaction_id{0};
  std::uint64_t statement_snapshot_id{0};
  std::string metadata_snapshot_uuid;
  bool transaction_active{false};
  bool statement_snapshot_fixed{false};
  bool engine_owned{false};
  bool finality_authority_claimed{false};
};

struct CanonicalOptimizerPolicyCapabilitySnapshot {
  std::string policy_snapshot_uuid;
  std::uint64_t policy_epoch{0};
  std::string capability_snapshot_uuid;
  std::uint32_t capability_abi_version{0};
  std::vector<planner::CanonicalLogicalRelationalNodeKind>
      supported_node_kinds;
  bool engine_owned{false};
  bool cluster_capability_claimed{false};
};

struct CanonicalOptimizerResourceSnapshot {
  std::string resource_snapshot_uuid;
  std::uint64_t resource_epoch{0};
  std::uint64_t memory_budget_bytes{0};
  std::uint64_t maximum_candidate_count{0};
  std::uint64_t maximum_memo_groups{0};
  std::uint64_t maximum_search_steps{0};
  std::uint64_t maximum_planning_time_ns{0};
  bool spill_allowed{false};
  bool engine_owned{false};
};

struct CanonicalOptimizerRouteSnapshot {
  std::string route_snapshot_uuid;
  std::uint64_t route_epoch{0};
  std::uint64_t route_generation{0};
  std::string operation_id;
  std::string route_id;
  bool native_local_route{false};
  bool engine_owned{false};
  bool cluster_route_claimed{false};
};

struct CanonicalOptimizerAdmissionRequest {
  std::uint16_t abi_version{1};
  planner::CanonicalLogicalRelationalGraph logical_graph;
  planner::CanonicalLogicalPropertyCatalog logical_properties;
  CanonicalOptimizerCatalogSnapshot catalog;
  CanonicalOptimizerSecuritySnapshot security;
  CanonicalOptimizerMgaSnapshot mga;
  CanonicalOptimizerPolicyCapabilitySnapshot policy_capability;
  CanonicalOptimizerResourceSnapshot resource;
  CanonicalOptimizerStatisticsSnapshot statistics;
  CanonicalOptimizerRouteSnapshot route;
  bool populated_from_admitted_typed_sblr{false};
  bool data_access_observed{false};
  bool parser_planning_authority_claimed{false};
};

struct CanonicalOptimizerAdmissionEvidence {
  CanonicalOptimizerAdmissionStage stage{
      CanonicalOptimizerAdmissionStage::kBoundRequest};
  std::string evidence_id;
};

struct CanonicalOptimizerAdmissionIssue {
  CanonicalOptimizerAdmissionStage stage{
      CanonicalOptimizerAdmissionStage::kBoundRequest};
  std::string diagnostic_id;
  std::string field_id;
};

struct CanonicalOptimizerAdmissionResult {
  bool admitted{false};
  bool planning_allowed{false};
  bool degraded_for_unknown_statistics{false};
  bool benchmark_clean_ready{false};
  bool data_access_allowed{false};
  std::string bound_sblr_tree_uuid;
  std::string catalog_epoch_uuid;
  std::string security_context_uuid;
  std::string capability_snapshot_uuid;
  std::string resource_snapshot_uuid;
  std::string statistics_snapshot_uuid;
  std::string route_snapshot_uuid;
  std::uint64_t local_transaction_id{0};
  std::uint64_t statement_snapshot_id{0};
  std::uint64_t catalog_generation{0};
  std::uint64_t security_epoch{0};
  std::uint64_t policy_epoch{0};
  std::uint64_t resource_epoch{0};
  std::uint64_t statistics_generation{0};
  std::uint64_t route_epoch{0};
  std::uint64_t route_generation{0};
  std::vector<CanonicalOptimizerAdmissionEvidence> evidence;
  std::vector<CanonicalOptimizerAdmissionIssue> issues;
};

struct CanonicalNativeObjectFreeAdmissionContext {
  std::string statement_uuid;
  std::string catalog_snapshot_uuid;
  std::string security_context_uuid;
  std::uint64_t catalog_generation{0};
  std::uint64_t authorization_catalog_generation{0};
  std::uint64_t security_epoch{0};
  std::uint64_t policy_epoch{0};
  std::uint64_t resource_epoch{0};
  std::string capability_snapshot_uuid;
  std::string resource_snapshot_uuid;
  std::string route_snapshot_uuid;
  std::uint64_t route_epoch{0};
  std::uint64_t route_generation{0};
  std::uint64_t memory_budget_bytes{0};
  std::uint64_t maximum_candidate_count{0};
  std::uint64_t maximum_memo_groups{0};
  std::uint64_t maximum_search_steps{0};
  std::uint64_t maximum_planning_time_ns{0};
  bool spill_allowed{false};
  std::uint64_t local_transaction_id{0};
  std::uint64_t statement_snapshot_id{0};
  std::uint64_t admitted_at_monotonic_ns{0};
  bool metadata_snapshot_engine_owned{false};
  bool authorization_context_engine_owned{false};
};

struct CanonicalNativeAdmissionBuildResult {
  bool built{false};
  CanonicalOptimizerAdmissionRequest request;
  CanonicalOptimizerAdmissionResult admission;
  std::string diagnostic_id;
  std::string field_id;
};

// QOW-SOURCE-OPT-006-CATALOG-V1
// QOW-SOURCE-OPT-006-SECURITY-V1
// QOW-SOURCE-OPT-006-MGA-V1
// QOW-SOURCE-OPT-006-POLICY-V1
// QOW-SOURCE-OPT-006-RESOURCE-V1
// QOW-SOURCE-OPT-006-ROUTE-V1
// QOW-SOURCE-OPT-006-STATISTICS-V1
CanonicalOptimizerAdmissionResult AdmitCanonicalOptimizerPlanningRequest(
    const CanonicalOptimizerAdmissionRequest& request);

CanonicalNativeAdmissionBuildResult
BuildCanonicalObjectFreeNativeOptimizerAdmissionRequest(
    const planner::CanonicalLogicalRelationalGraph& graph,
    const planner::CanonicalLogicalPropertyCatalog& properties,
    const CanonicalNativeObjectFreeAdmissionContext& context);

#ifndef SCRATCHBIRD_QOW_CANONICAL_CANDIDATE_LEGALITY_ONLY
// SEARCH_KEY: PCR061_CATALOG_BACKED_PRODUCTION_PLANNING
// Production optimizer admission requires catalog-backed table/index
// statistics, descriptor/route/security/redaction/memory dependency digests,
// and engine-owned SBLR/MGA/security context. Local/default statistics are
// diagnostic fallback evidence only and cannot satisfy production admission.
struct CatalogBackedProductionPlanningRequest {
  BoundOptimizerRequest bound_request;
  OptimizerPlanCacheKeyInput plan_cache_key_input;
  bool production_build = true;
  bool require_index_stats = true;
};

struct CatalogBackedProductionPlanningValidation {
  bool ok = false;
  bool benchmark_clean_ready = false;
  bool catalog_backed = false;
  bool local_or_policy_default_diagnostic_only = false;
  std::string diagnostic_code;
  std::vector<std::string> diagnostics;
  std::vector<std::string> evidence;
};

struct CatalogBackedProductionPlanningResult {
  CatalogBackedProductionPlanningValidation validation;
  OptimizedPlan optimized_plan;
  BoundOptimizerResult bound_result;
};

CatalogBackedProductionPlanningValidation
ValidateCatalogBackedProductionPlanningRequest(
    const CatalogBackedProductionPlanningRequest& request);

CatalogBackedProductionPlanningResult OptimizeCatalogBackedProductionPlan(
    const CatalogBackedProductionPlanningRequest& request);
#endif

}  // namespace scratchbird::engine::optimizer
