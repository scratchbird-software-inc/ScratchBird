// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "ipar_index_support_services.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::index {
namespace {

bool Safe(const IparIndexAuthorityBoundary& authority) {
  return authority.durable_transaction_inventory_authority &&
         !authority.support_service_visibility_authority &&
         !authority.background_agent_finality_authority &&
         !authority.parser_finality_authority;
}

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

}  // namespace

IparIndexMaintenancePlan SelectIparIndexMaintenanceRoute(
    const IparIndexMaintenanceRequest& request) {
  IparIndexMaintenancePlan plan;
  Add(&plan.evidence, "IPAR-P4-07");
  if (!Safe(request.authority) || request.index_uuid.empty() ||
      request.family.empty() || request.row_count == 0) {
    plan.diagnostic_code = "IPAR_INDEX_ROUTE_UNSAFE";
    return plan;
  }
  plan.accepted = true;
  if (request.unique || request.foreground_exactness_required) {
    plan.route = IparIndexMaintenanceRoute::exact_unique_probe;
    plan.exact_visibility_recheck = true;
    plan.async_background_work = false;
    plan.diagnostic_code = "IPAR_INDEX_ROUTE_EXACT_SYNC";
    Add(&plan.evidence, "unique_or_exact_route=synchronous_probe");
    return plan;
  }
  if (request.bulk_load && request.shadow_build_allowed) {
    plan.route = IparIndexMaintenanceRoute::sorted_shadow_build;
    plan.async_background_work = true;
    plan.diagnostic_code = "IPAR_INDEX_ROUTE_SORTED_SHADOW_BUILD";
    Add(&plan.evidence, "shadow_publish_requires_mga_authorization=true");
    return plan;
  }
  if (request.committed_delta_available && request.delta_count <= request.row_count) {
    plan.route = IparIndexMaintenanceRoute::committed_delta_overlay;
    plan.async_background_work = true;
    plan.diagnostic_code = "IPAR_INDEX_ROUTE_COMMITTED_DELTA_OVERLAY";
    Add(&plan.evidence, "non_unique_overlay_committed_only=true");
    return plan;
  }
  plan.route = IparIndexMaintenanceRoute::background_delta_merge;
  plan.async_background_work = true;
  plan.diagnostic_code = "IPAR_INDEX_ROUTE_BACKGROUND_MERGE";
  Add(&plan.evidence, "background_merge_visibility_authority=false");
  return plan;
}

IparIndexSupportPlan PlanIparIndexSupportWork(
    const std::vector<IparIndexSupportWorkItem>& items,
    const IparIndexAuthorityBoundary& authority) {
  IparIndexSupportPlan plan;
  Add(&plan.evidence, "IPAR-P4-10");
  if (!Safe(authority)) {
    plan.diagnostic_code = "IPAR_INDEX_SUPPORT_AUTHORITY_UNSAFE";
    return plan;
  }
  for (const auto& item : items) {
    if (item.index_uuid.empty() || item.work_kind.empty() || item.bytes == 0 ||
        !item.committed_only) {
      plan.diagnostic_code = "IPAR_INDEX_SUPPORT_ITEM_UNSAFE";
      return plan;
    }
    if (item.work_kind == "delta_merge") ++plan.merge_count;
    if (item.work_kind == "compaction") ++plan.compaction_count;
    if (item.work_kind == "post_ddl_validation") ++plan.validation_count;
  }
  plan.accepted = true;
  plan.diagnostic_code = "IPAR_INDEX_SUPPORT_PLAN_READY";
  Add(&plan.evidence, "index_support_committed_rows_only=true");
  Add(&plan.evidence, "post_ddl_index_validation_present=true");
  return plan;
}

IparIndexSplitReservePlan PlanIparIndexSplitReserve(
    const IparIndexSplitPredictorRequest& request) {
  IparIndexSplitReservePlan plan;
  Add(&plan.evidence, "IPAR-P4-12");
  if (!Safe(request.authority) || request.index_uuid.empty() ||
      request.page_size == 0 || request.average_key_bytes == 0 ||
      request.incoming_keys == 0) {
    plan.diagnostic_code = "IPAR_INDEX_SPLIT_REQUEST_UNSAFE";
    return plan;
  }
  const auto incoming_bytes = request.average_key_bytes * request.incoming_keys;
  plan.split_predicted =
      incoming_bytes > request.current_free_bytes || request.monotonic_hot_range;
  plan.reserve_pages = plan.split_predicted
                           ? std::max<std::uint64_t>(
                                 1, (incoming_bytes + request.page_size - 1) /
                                        request.page_size)
                           : 0;
  if (request.monotonic_hot_range) {
    plan.reserve_pages = std::max<std::uint64_t>(plan.reserve_pages, 2);
  }
  plan.refill_threshold_bytes = request.page_size / 4;
  plan.accepted = true;
  plan.diagnostic_code = "IPAR_INDEX_SPLIT_RESERVE_READY";
  Add(&plan.evidence, "split_reserve_foreground_stall_reduced=true");
  return plan;
}

IparProbePrefetchPlan PlanIparProbePrefetch(
    const IparProbePrefetchRequest& request) {
  IparProbePrefetchPlan plan;
  Add(&plan.evidence, "IPAR-P4-13");
  if (!Safe(request.authority) || request.constraint_uuid.empty() ||
      request.referenced_index_uuid.empty() || request.keys.empty() ||
      !(request.foreign_key || request.unique) || !request.security_epoch_present ||
      !request.snapshot_visibility_recheck_required) {
    plan.diagnostic_code = "IPAR_PROBE_PREFETCH_UNSAFE";
    return plan;
  }
  plan.accepted = true;
  plan.prefetch_scheduled = true;
  plan.exact_probe_required = true;
  plan.probe_page_count =
      std::max<std::uint64_t>(1, (request.keys.size() + 15) / 16);
  plan.diagnostic_code = "IPAR_PROBE_PREFETCH_READY";
  Add(&plan.evidence, "prefetch_exact_visibility_recheck=true");
  Add(&plan.evidence, request.foreign_key ? "foreign_key_probe=true"
                                          : "unique_probe=true");
  return plan;
}

}  // namespace scratchbird::core::index
