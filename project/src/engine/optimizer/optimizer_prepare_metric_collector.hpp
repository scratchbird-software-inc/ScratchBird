// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "optimizer_plan_cache.hpp"

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace scratchbird::engine::optimizer {

struct CanonicalPrepareMetricCollectionContext {
  std::string leg_uuid;
  std::string family_id;
  std::vector<std::string> required_metric_ids;
  std::vector<CanonicalPreparedLegPlanReceipt> completed_dependency_plans;
  std::uint64_t deadline_monotonic_ns{0};
  std::function<bool()> cancellation_requested;
};

struct CanonicalPrepareMetricCollectionOutput {
  bool collected{false};
  std::string metric_snapshot_uuid;
  std::uint64_t metric_snapshot_generation{0};
  std::vector<CanonicalPreparedMetricValue> metrics;
  std::string diagnostic_id;
  std::string detail;
  bool parser_execution_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalPrepareLegPlanningContext {
  CanonicalPreparedMetricCollectionReceipt metric_receipt;
  std::vector<CanonicalPreparedLegPlanReceipt> completed_dependency_plans;
  std::uint64_t deadline_monotonic_ns{0};
  std::function<bool()> cancellation_requested;
};

struct CanonicalPrepareLegPlanningOutput {
  bool planned{false};
  std::string selected_leg_plan_uuid;
  std::string selected_alternative_uuid;
  std::string family_local_cost_vector_uuid;
  std::vector<std::string> retained_alternative_uuids;
  std::uint64_t estimated_output_rows{0};
  std::string diagnostic_id;
  std::string detail;
  bool family_local_selection{true};
  bool cross_family_cost_comparison_performed{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalPrepareMetricLegRequest {
  std::string leg_uuid;
  std::string family_id;
  std::vector<std::string> dependency_leg_uuids;
  std::vector<std::string> required_metric_ids;
  std::function<CanonicalPrepareMetricCollectionOutput(
      const CanonicalPrepareMetricCollectionContext&)>
      collect_metrics;
  std::function<CanonicalPrepareLegPlanningOutput(
      const CanonicalPrepareLegPlanningContext&)>
      plan_leg;
  // Called once for every started leg, including refusal, cancellation, and
  // timeout paths. It must release all collector/planner transient state.
  std::function<bool()> cleanup_transient_state;
};

struct CanonicalPrepareWithMetricCollectionRequest {
  std::string coordinator_policy_uuid;
  std::uint64_t coordinator_policy_generation{0};
  std::string bound_sblr_tree_uuid;
  std::string route_snapshot_uuid;
  std::uint64_t route_epoch{0};
  std::uint64_t route_generation{0};
  std::string cluster_scope_id;
  std::uint16_t metric_thread_budget{0};
  std::uint64_t timeout_ns{0};
  std::vector<CanonicalPrepareMetricLegRequest> legs;
  std::function<bool()> cancellation_requested;
  std::function<CanonicalPreparePhysicalPlanRequest(
      const CanonicalPreparedMetricCoordinatorReceipt&,
      const std::vector<CanonicalPreparedMetricCollectionReceipt>&,
      const std::vector<CanonicalPreparedLegPlanReceipt>&)>
      assemble_selected_plan;
  bool engine_prepare_authorized{false};
  bool global_security_admitted{false};
  bool global_mga_admitted{false};
  bool parser_execution_authority_claimed{false};
  bool transaction_visibility_authority_claimed{false};
  bool transaction_finality_authority_claimed{false};
  bool recovery_authority_claimed{false};
};

struct CanonicalPrepareWithMetricCollectionIssue {
  std::string diagnostic_id;
  std::string field_id;
  std::string detail;
};

struct CanonicalPrepareWithMetricCollectionResult {
  bool accepted{false};
  bool metrics_collected{false};
  bool legs_planned{false};
  bool prepared{false};
  bool persisted{false};
  bool cancelled{false};
  bool timed_out{false};
  bool all_workers_joined{false};
  bool transient_state_cleaned{false};
  std::uint16_t maximum_observed_concurrency{0};
  std::uint64_t metric_collector_invocation_count{0};
  std::uint64_t leg_planner_invocation_count{0};
  std::uint64_t cleanup_invocation_count{0};
  CanonicalPreparePhysicalPlanResult prepare_result;
  std::vector<CanonicalPreparedMetricCollectionReceipt> metric_receipts;
  std::vector<CanonicalPreparedLegPlanReceipt> leg_plan_receipts;
  std::vector<CanonicalPrepareWithMetricCollectionIssue> issues;
};

// Returns the exact required metric inventory for a canonical model-family
// leg. The inventory is sorted and is part of admission, not a suggestion.
std::vector<std::string> CanonicalRequiredPrepareMetricIdsForFamily(
    const std::string& family_id);

CanonicalPrepareWithMetricCollectionResult
PrepareCanonicalPhysicalPlanWithMetricCollection(
    const CanonicalPrepareWithMetricCollectionRequest& request,
    CanonicalPreparedPlanStore* prepared_plan_store);

}  // namespace scratchbird::engine::optimizer
