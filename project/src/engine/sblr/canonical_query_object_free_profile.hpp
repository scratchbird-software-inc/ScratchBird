// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "canonical_query_execute.hpp"
#include "engine/executor/executor_foundation.hpp"
#include "engine/optimizer/relational_planner.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace scratchbird::engine::sblr {

namespace exec = scratchbird::engine::executor;
namespace plan = scratchbird::engine::planner;

// Object-free planning publishes only immutable capability, cost, property,
// and bounded runtime-memory facts. It does not execute a physical node,
// resolve visibility, or own transaction finality.
struct LivePhysicalNodeProfile {
  std::uint32_t logical_node_id{0};
  std::string implementation_id;
  std::string capability_uuid;
  plan::CanonicalLogicalRelationalNodeKind logical_node_kind{
      plan::CanonicalLogicalRelationalNodeKind::kValues};
  exec::PhysicalNodeKind physical_node_kind{exec::PhysicalNodeKind::kValues};
  std::string transformation_rule_id;
  std::uint64_t estimated_rows{0};
  std::uint64_t memory_bytes_required{0};
  std::size_t minimum_input_count{0};
  std::size_t maximum_input_count{0};
  std::vector<std::string> required_property_uuids;
  std::vector<std::string> delivered_property_uuids;
  std::vector<plan::CanonicalLogicalPropertyKind> supported_property_kinds;
  std::uint64_t page_read_sequential_units{0};
  std::uint64_t page_read_random_units{0};
  std::uint64_t page_write_units{0};
  std::uint64_t cache_units{0};
  std::uint64_t memory_grant_units{0};
  std::uint64_t spill_units{0};
  std::uint64_t network_units{0};
  std::uint64_t compression_units{0};
  std::uint64_t encryption_units{0};
  std::uint64_t predicate_evaluation_units{0};
  std::uint64_t vector_distance_units{0};
  std::uint64_t text_scoring_units{0};
  std::uint64_t spatial_evaluation_units{0};
  std::uint64_t udr_invocation_units{0};
  std::uint64_t mga_units{0};
  std::uint64_t index_maintenance_units{0};
  std::uint64_t mga_visibility_checks_expected{0};
  bool storage_read_capable{false};
  bool mga_visibility_capable{false};
  bool spill_supported{false};
  bool parallel_safe{false};
  bool parallel_required{false};
  bool residual_predicate_required{false};
  bool storage_recheck_required{false};
  std::string compatibility_profile_id{"native.sblr.row.v1"};
  std::uint64_t runtime_accounted_auxiliary_memory_bytes{0};
  std::uint64_t runtime_producer_peak_memory_bytes{0};
  bool runtime_producer_peak_memory_exact{false};
  bool runtime_peak_from_callback_batches{false};
  bool runtime_auxiliary_from_first_input_batch{false};
  std::string model_family_id;
};

struct OrdinaryRuntimeMemoryReceipt {
  std::uint64_t physical_node_id{0};
  std::string implementation_id;
  std::uint64_t producer_peak_memory_bytes{0};
  bool producer_peak_memory_exact{false};
  bool producer_peak_from_runtime_batches{false};
  std::uint64_t accounted_auxiliary_memory_bytes{0};
  bool accounted_auxiliary_from_first_input_batch{false};
  bool residual_recheck_applicable{false};
  bool non_accessing_proven{false};
};

using OrdinaryRuntimeMemoryReceipts =
    std::unordered_map<std::uint64_t, OrdinaryRuntimeMemoryReceipt>;

struct LivePhysicalPlanningResult {
  bool ok{false};
  exec::TypedPhysicalNodeDag physical_dag;
  OrdinaryRuntimeMemoryReceipts ordinary_runtime_memory_receipts;
  std::string diagnostic_id;
  std::string detail;
};

bool CompleteLiveRuntimeMemoryReceipts(
    std::vector<LivePhysicalNodeProfile>* profiles,
    const std::vector<std::pair<std::uint32_t, std::uint64_t>>&
        auxiliary_terms = {});

LivePhysicalPlanningResult PlanAndPublishLivePhysicalDag(
    const CanonicalObjectFreeValuesExecutionRequest& request,
    const std::vector<LivePhysicalNodeProfile>& profiles,
    std::string_view selected_plan_purpose,
    std::string_view operation_name,
    std::string_view legacy_model_family_hint = "relational.local.v1");

}  // namespace scratchbird::engine::sblr
