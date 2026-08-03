// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "optimizer_catalog_backed_planning.hpp"

#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

namespace opt = scratchbird::engine::optimizer;
namespace plan = scratchbird::engine::planner;

namespace {

constexpr std::string_view kTree =
    "019f0000-0000-7300-8000-000000006101";
constexpr std::string_view kCatalog =
    "019f0000-0000-7300-8000-000000006102";
constexpr std::string_view kMetadata =
    "019f0000-0000-7300-8000-000000006109";
constexpr std::string_view kSecurity =
    "019f0000-0000-7300-8000-000000006103";
constexpr std::string_view kRelation =
    "019f0000-0000-7300-8000-000000006104";
constexpr std::string_view kStatistics =
    "019f0000-0000-7300-8000-000000006105";
constexpr std::uint64_t kOwner = 0xffff'ffff'ffff'ff00ULL;
constexpr std::uint64_t kOldestActive = 0xffff'ffff'ffff'fee8ULL;
constexpr std::uint64_t kHorizon = 0xffff'ffff'ffff'fed0ULL;
constexpr std::uint64_t kInDoubt = 0xffff'ffff'ffff'fef0ULL;
constexpr std::uint64_t kInventoryNext = 0xffff'ffff'ffff'fff0ULL;

bool Require(const bool condition, const std::string_view detail) {
  if (!condition) {
    std::cerr << "QOW-TEST-OPT-006-CATALOG-V1: " << detail << '\n';
  }
  return condition;
}

plan::CanonicalMgaStatementContext MgaContext() {
  plan::CanonicalMgaStatementContext context;
  context.statement_uuid =
      "019f0000-0000-7300-8000-000000006110";
  context.owning_transaction_uuid =
      "019f0000-0000-7300-8000-000000006111";
  context.statement_snapshot_uuid =
      "019f0000-0000-7300-8000-000000006112";
  context.statement_metadata_snapshot_uuid = std::string(kMetadata);
  context.owning_local_transaction_id = kOwner;
  context.visible_committed_high_watermark = 0;
  context.oldest_active_transaction_id = kOldestActive;
  context.oldest_interesting_transaction_id = kHorizon;
  context.oldest_snapshot_transaction_id = kHorizon;
  context.retention_horizon_transaction_id = kHorizon;
  context.active_excluded_local_transaction_ids = {kOldestActive, kOwner};
  context.in_doubt_excluded_local_transaction_ids = {kInDoubt};
  context.snapshot_kind = "statement_stable";
  context.publication_inventory_next_local_transaction_id = kInventoryNext;
  context.inventory_authoritative = true;
  context.complete = true;
  context.current = true;
  return context;
}

opt::CanonicalOptimizerAdmissionRequest Request() {
  opt::CanonicalOptimizerAdmissionRequest request;
  auto& graph = request.logical_graph;
  graph.bound_sblr_tree_uuid = std::string(kTree);
  graph.catalog_epoch_uuid = std::string(kCatalog);
  graph.security_context_uuid = std::string(kSecurity);
  graph.local_transaction_id = kOwner;
  graph.statement_snapshot_id = 0;
  graph.mga_statement_context = MgaContext();
  graph.root_logical_node_id = 1;
  graph.result_descriptor_ids = {1};
  plan::CanonicalLogicalRelationalNode source;
  source.logical_node_id = 1;
  source.node_kind = plan::CanonicalLogicalRelationalNodeKind::kRelationSource;
  source.output_descriptor_ids = {1};
  source.origin_relational_node_ids = {1};
  source.required_object_uuids = {std::string(kRelation)};
  source.semantic_variant_id = "relation.source.v1";
  graph.nodes = {source};

  request.logical_properties.bound_sblr_tree_uuid = std::string(kTree);
  request.logical_properties.catalog_epoch_uuid = std::string(kCatalog);
  request.logical_properties.security_context_uuid = std::string(kSecurity);
  request.logical_properties.local_transaction_id = kOwner;
  request.logical_properties.statement_snapshot_id = 0;
  request.logical_properties.mga_statement_context = MgaContext();

  request.catalog.snapshot_uuid = std::string(kMetadata);
  request.catalog.catalog_epoch_uuid = std::string(kCatalog);
  request.catalog.catalog_generation = 17;
  request.catalog.object_uuids = {std::string(kRelation)};
  request.catalog.descriptor_ids = {1};
  request.catalog.engine_owned = true;

  request.security.security_context_uuid = std::string(kSecurity);
  request.security.security_epoch = 18;
  request.security.policy_epoch = 19;
  request.security.catalog_generation = 17;
  request.security.authorized_object_uuids = {std::string(kRelation)};
  request.security.engine_owned = true;

  request.mga.local_transaction_id = kOwner;
  request.mga.statement_snapshot_id = 0;
  request.mga.statement_context = MgaContext();
  request.mga.metadata_snapshot_uuid = std::string(kMetadata);
  request.mga.transaction_active = true;
  request.mga.statement_snapshot_fixed = true;
  request.mga.engine_owned = true;

  request.policy_capability.policy_snapshot_uuid = std::string(kSecurity);
  request.policy_capability.policy_epoch = 19;
  request.policy_capability.capability_snapshot_uuid =
      "019f0000-0000-7300-8000-000000006106";
  request.policy_capability.capability_abi_version = 1;
  request.policy_capability.supported_node_kinds = {
      plan::CanonicalLogicalRelationalNodeKind::kRelationSource};
  request.policy_capability.engine_owned = true;

  request.resource.resource_snapshot_uuid =
      "019f0000-0000-7300-8000-000000006107";
  request.resource.resource_epoch = 20;
  request.resource.memory_budget_bytes = 4 * 1024 * 1024;
  request.resource.maximum_candidate_count = 8;
  request.resource.maximum_memo_groups = 8;
  request.resource.maximum_search_steps = 64;
  request.resource.maximum_planning_time_ns = 1'000'000;
  request.resource.spill_allowed = true;
  request.resource.engine_owned = true;

  request.statistics.statistics_snapshot_uuid = std::string(kStatistics);
  request.statistics.catalog_epoch_uuid = std::string(kCatalog);
  request.statistics.statistics_generation = 21;
  request.statistics.admitted_at_monotonic_ns = 1'000'000;
  request.statistics.captured_before_data_access = true;
  opt::CanonicalOptimizerNodeEstimate estimate;
  estimate.logical_node_id = 1;
  estimate.object_uuid = std::string(kRelation);
  estimate.state = opt::CanonicalOptimizerStatisticState::kKnown;
  estimate.source = opt::CanonicalOptimizerStatisticSource::kCatalogExact;
  estimate.catalog_epoch_uuid = std::string(kCatalog);
  estimate.statistics_snapshot_uuid = std::string(kStatistics);
  estimate.statistics_generation = 21;
  estimate.collected_at_monotonic_ns = 900'000;
  estimate.admitted_at_monotonic_ns = 1'000'000;
  estimate.maximum_age_ns = 200'000;
  estimate.confidence = opt::CostConfidence::kExact;
  estimate.row_count_present = true;
  estimate.row_count = 0;
  estimate.page_count_present = true;
  estimate.page_count = 1;
  request.statistics.node_estimates = {estimate};

  request.route.route_snapshot_uuid =
      "019f0000-0000-7300-8000-000000006108";
  request.route.route_epoch = 22;
  request.route.route_generation = 23;
  request.route.operation_id = "query.execute";
  request.route.route_id = "native.sblr.query.execute.v2";
  request.route.native_local_route = true;
  request.route.engine_owned = true;
  request.populated_from_admitted_typed_sblr = true;
  return request;
}

bool ValidateCatalogAdmission() {
  const auto result = opt::AdmitCanonicalOptimizerPlanningRequest(Request());
  bool passed = true;
  passed &= Require(result.admitted && result.planning_allowed &&
                        result.benchmark_clean_ready &&
                        !result.degraded_for_unknown_statistics &&
                        !result.data_access_allowed && result.issues.empty() &&
                        result.evidence.size() == 8 &&
                        result.local_transaction_id == kOwner &&
                        result.statement_snapshot_id == 0 &&
                        plan::CanonicalMgaStatementContextEqual(
                            result.mga_statement_context, MgaContext()),
                    "complete catalog-backed request was not admitted");
  for (std::size_t index = 0; index < result.evidence.size(); ++index) {
    passed &= Require(
        result.evidence[index].stage ==
            static_cast<opt::CanonicalOptimizerAdmissionStage>(index + 1),
        "admission evidence order changed");
  }
  return passed;
}

bool ValidateCatalogRefusals() {
  bool passed = true;
  const auto expect_catalog_refusal = [&](auto mutation,
                                           const std::string_view detail) {
    auto request = Request();
    mutation(request);
    const auto result = opt::AdmitCanonicalOptimizerPlanningRequest(request);
    return Require(!result.admitted && !result.planning_allowed &&
                       !result.data_access_allowed &&
                       result.evidence.size() == 1 &&
                       result.issues.size() == 1 &&
                       result.issues.front().stage ==
                           opt::CanonicalOptimizerAdmissionStage::kCatalogEpoch &&
                       result.issues.front().diagnostic_id ==
                           "QOW-DIAG-OPTIMIZER-ADMISSION-CATALOG-V1",
                   detail);
  };
  passed &= expect_catalog_refusal(
      [](auto& request) { request.catalog.engine_owned = false; },
      "non-engine catalog snapshot was admitted");
  passed &= expect_catalog_refusal(
      [](auto& request) { request.catalog.object_uuids.clear(); },
      "missing catalog object was admitted");
  passed &= expect_catalog_refusal(
      [](auto& request) { request.catalog.descriptor_ids.clear(); },
      "missing descriptor snapshot was admitted");
  passed &= expect_catalog_refusal(
      [](auto& request) {
        request.catalog.catalog_epoch_uuid =
            "019f0000-0000-7300-8000-000000006999";
      },
      "stale catalog epoch was admitted");
  passed &= expect_catalog_refusal(
      [](auto& request) {
        request.catalog.catalog_epoch_uuid =
            "00000000-0000-0000-0000-000000000000";
      },
      "nil catalog epoch was admitted");
  passed &= expect_catalog_refusal(
      [](auto& request) {
        request.catalog.snapshot_uuid = request.catalog.catalog_epoch_uuid;
      },
      "catalog epoch was accepted as the metadata snapshot");
  passed &= expect_catalog_refusal(
      [](auto& request) {
        std::swap(request.catalog.snapshot_uuid,
                  request.catalog.catalog_epoch_uuid);
      },
      "swapped catalog epoch and metadata snapshot were admitted");

  const auto expect_mga_refusal = [&](auto mutation,
                                      const std::string_view detail) {
    auto request = Request();
    mutation(request);
    const auto result = opt::AdmitCanonicalOptimizerPlanningRequest(request);
    return Require(!result.admitted && !result.planning_allowed &&
                       !result.issues.empty() &&
                       result.issues.front().stage ==
                           opt::CanonicalOptimizerAdmissionStage::
                               kMgaStatementBoundary,
                   detail);
  };
  passed &= expect_mga_refusal(
      [](auto& request) {
        request.mga.statement_context.current = false;
      },
      "non-current inventory snapshot was admitted");
  passed &= expect_mga_refusal(
      [](auto& request) {
        request.mga.statement_context.oldest_active_transaction_id =
            kInventoryNext;
      },
      "future snapshot horizon was admitted");
  passed &= expect_mga_refusal(
      [](auto& request) {
        request.mga.statement_context
            .active_excluded_local_transaction_ids =
                {kOldestActive, kOwner, kInventoryNext};
      },
      "out-of-ceiling snapshot exclusion was admitted");
  return passed;
}

bool ValidateCompleteMgaCarrierRefusals() {
  const auto expect_refusal = [](auto mutation,
                                 const opt::CanonicalOptimizerAdmissionStage stage,
                                 const std::size_t evidence_count,
                                 const std::string_view detail) {
    auto request = Request();
    mutation(request);
    const auto result = opt::AdmitCanonicalOptimizerPlanningRequest(request);
    return Require(!result.admitted && !result.planning_allowed &&
                       !result.data_access_allowed &&
                       result.evidence.size() == evidence_count &&
                       result.issues.size() == 1 &&
                       result.issues.front().stage == stage,
                   detail);
  };
  using Stage = opt::CanonicalOptimizerAdmissionStage;
  bool passed = true;
  passed &= expect_refusal(
      [](auto& request) {
        request.logical_graph.mga_statement_context.complete = false;
      }, Stage::kBoundRequest, 0,
      "incomplete logical-graph statement context reached admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.logical_properties.mga_statement_context.current = false;
      }, Stage::kBoundRequest, 0,
      "stale logical-property statement context reached admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.logical_graph.local_transaction_id =
            static_cast<std::uint32_t>(kOwner);
      }, Stage::kBoundRequest, 0,
      "narrowed graph transaction alias reached admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.statement_context.statement_uuid.clear();
      }, Stage::kMgaStatementBoundary, 3,
      "missing statement UUID reached MGA admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.statement_context.owning_transaction_uuid =
            "019F0000-0000-7300-8000-000000006111";
      }, Stage::kMgaStatementBoundary, 3,
      "malformed owner UUID reached MGA admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.statement_context.in_doubt_excluded_local_transaction_ids =
            {kInDoubt, kInDoubt};
      }, Stage::kMgaStatementBoundary, 3,
      "duplicate in-doubt exclusion reached MGA admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.statement_context.in_doubt_excluded_local_transaction_ids =
            {kOwner};
      }, Stage::kMgaStatementBoundary, 3,
      "overlapping exclusion vectors reached MGA admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.statement_context.publication_inventory_next_local_transaction_id =
            static_cast<std::uint32_t>(kInventoryNext);
      }, Stage::kMgaStatementBoundary, 3,
      "truncated inventory ceiling reached MGA admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.local_transaction_id =
            static_cast<std::uint32_t>(kOwner);
      }, Stage::kMgaStatementBoundary, 3,
      "narrowed MGA transaction alias reached admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.statement_context.statement_snapshot_uuid =
            "019f0000-0000-7300-8000-000000006998";
      }, Stage::kMgaStatementBoundary, 3,
      "swapped MGA statement context reached admission");
  passed &= expect_refusal(
      [](auto& request) {
        request.mga.metadata_snapshot_uuid =
            request.catalog.catalog_epoch_uuid;
      }, Stage::kMgaStatementBoundary, 3,
      "catalog epoch was accepted as MGA metadata snapshot");
  return passed;
}

}  // namespace

#ifndef QOW_OPT_006_FIXTURE_ONLY
// QOW-TEST-OPT-006-CATALOG-V1
int main() {
  bool passed = true;
  passed &= ValidateCatalogAdmission();
  passed &= ValidateCatalogRefusals();
  passed &= ValidateCompleteMgaCarrierRefusals();
  return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
#endif
