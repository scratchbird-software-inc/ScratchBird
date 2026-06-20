// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace scratchbird::core::index {

struct IparIndexAuthorityBoundary {
  bool durable_transaction_inventory_authority = true;
  bool support_service_visibility_authority = false;
  bool background_agent_finality_authority = false;
  bool parser_finality_authority = false;
};

enum class IparIndexMaintenanceRoute {
  exact_unique_probe,
  committed_delta_overlay,
  background_delta_merge,
  sorted_shadow_build
};

struct IparIndexMaintenanceRequest {
  std::string index_uuid;
  std::string family;
  bool unique = false;
  bool bulk_load = false;
  bool committed_delta_available = false;
  bool shadow_build_allowed = false;
  bool foreground_exactness_required = true;
  std::uint64_t row_count = 0;
  std::uint64_t delta_count = 0;
  IparIndexAuthorityBoundary authority;
};

struct IparIndexMaintenancePlan {
  bool accepted = false;
  IparIndexMaintenanceRoute route = IparIndexMaintenanceRoute::exact_unique_probe;
  bool exact_visibility_recheck = true;
  bool async_background_work = false;
  bool publish_requires_mga_authorization = true;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

IparIndexMaintenancePlan SelectIparIndexMaintenanceRoute(
    const IparIndexMaintenanceRequest& request);

struct IparIndexSupportWorkItem {
  std::string index_uuid;
  std::string work_kind;
  std::uint64_t bytes = 0;
  bool committed_only = true;
};

struct IparIndexSupportPlan {
  bool accepted = false;
  std::uint64_t merge_count = 0;
  std::uint64_t compaction_count = 0;
  std::uint64_t validation_count = 0;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

IparIndexSupportPlan PlanIparIndexSupportWork(
    const std::vector<IparIndexSupportWorkItem>& items,
    const IparIndexAuthorityBoundary& authority);

struct IparIndexSplitPredictorRequest {
  std::string index_uuid;
  std::uint64_t page_size = 0;
  std::uint64_t current_free_bytes = 0;
  std::uint64_t average_key_bytes = 0;
  std::uint64_t incoming_keys = 0;
  bool monotonic_hot_range = false;
  IparIndexAuthorityBoundary authority;
};

struct IparIndexSplitReservePlan {
  bool accepted = false;
  bool split_predicted = false;
  std::uint64_t reserve_pages = 0;
  std::uint64_t refill_threshold_bytes = 0;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

IparIndexSplitReservePlan PlanIparIndexSplitReserve(
    const IparIndexSplitPredictorRequest& request);

struct IparProbePrefetchRequest {
  std::string constraint_uuid;
  std::string referenced_index_uuid;
  std::vector<std::string> keys;
  bool foreign_key = false;
  bool unique = false;
  bool security_epoch_present = true;
  bool snapshot_visibility_recheck_required = true;
  IparIndexAuthorityBoundary authority;
};

struct IparProbePrefetchPlan {
  bool accepted = false;
  bool exact_probe_required = true;
  bool prefetch_scheduled = false;
  std::uint64_t probe_page_count = 0;
  std::string diagnostic_code;
  std::vector<std::string> evidence;
};

IparProbePrefetchPlan PlanIparProbePrefetch(
    const IparProbePrefetchRequest& request);

}  // namespace scratchbird::core::index
