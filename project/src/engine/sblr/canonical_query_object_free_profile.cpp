// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "canonical_query_object_free_profile.hpp"

#include "canonical_query_scalar_support.hpp"

#include <algorithm>
#include <limits>
#include <ranges>
#include <unordered_set>

namespace scratchbird::engine::sblr {
namespace opt = scratchbird::engine::optimizer;

// SEARCH_KEY: SB_ENGINE_CANONICAL_QUERY_OBJECT_FREE_PROFILE_AUTHORITY
bool CompleteLiveRuntimeMemoryReceipts(
    std::vector<LivePhysicalNodeProfile>* profiles,
    const std::vector<std::pair<std::uint32_t, std::uint64_t>>&
        auxiliary_terms) {
  if (profiles == nullptr || profiles->empty()) return false;
  for (auto& profile : *profiles) {
    // Callback-derived profiles publish their actual input/output peak after
    // execution. Keep the pre-dispatch receipt internally consistent with
    // any statically known resident state until that exact value replaces it.
    profile.runtime_producer_peak_memory_bytes =
        profile.runtime_peak_from_callback_batches
            ? std::max<std::uint64_t>(
                  1, profile.runtime_accounted_auxiliary_memory_bytes)
            : profile.memory_bytes_required;
    profile.runtime_producer_peak_memory_exact = true;
  }
  for (const auto& [logical_node_id, bytes] : auxiliary_terms) {
    const auto profile = std::ranges::find_if(
        *profiles, [&](const auto& candidate) {
          return candidate.logical_node_id == logical_node_id;
        });
    if (profile == profiles->end() ||
        bytes > std::numeric_limits<std::uint64_t>::max() -
                    profile->runtime_accounted_auxiliary_memory_bytes) {
      return false;
    }
    profile->runtime_accounted_auxiliary_memory_bytes += bytes;
    if (profile->runtime_accounted_auxiliary_memory_bytes >
        profile->runtime_producer_peak_memory_bytes) {
      return false;
    }
  }
  return true;
}

LivePhysicalPlanningResult PlanAndPublishLivePhysicalDag(
    const CanonicalObjectFreeValuesExecutionRequest& request,
    const std::vector<LivePhysicalNodeProfile>& profiles,
    const std::string_view selected_plan_purpose,
    const std::string_view operation_name,
    const std::string_view /*legacy_model_family_hint*/) {
  LivePhysicalPlanningResult result;
  const auto& graph = request.optimizer_request.logical_graph;
  if (profiles.empty() ||
      profiles.size() >
          request.optimizer_request.resource.maximum_candidate_count) {
    result.diagnostic_id = "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1";
    result.detail = std::string(operation_name) +
                    " live profile inventory is empty or unbounded";
    return result;
  }

  const auto identity_scope =
      graph.bound_sblr_tree_uuid + ":" + request.context.statement_uuid;
  const auto calibration_uuid =
      DerivedCanonicalUuid(identity_scope, "relational.calibration");
  std::unordered_set<std::uint32_t> covered_nodes;
  opt::CanonicalOptimizerExecutorAvailability executor_availability;
  executor_availability.engine_owned = true;
  executor_availability.capability_catalog.capability_snapshot_uuid =
      request.optimizer_admission.capability_snapshot_uuid;
  executor_availability.capability_catalog.policy_epoch =
      request.optimizer_admission.policy_epoch;
  executor_availability.capability_catalog.engine_owned = true;
  executor_availability.node_bindings.reserve(profiles.size());
  std::unordered_map<std::string, std::size_t> capability_indexes;
  for (const auto& profile : profiles) {
    const auto node = std::ranges::find_if(graph.nodes, [&](const auto& item) {
      return item.logical_node_id == profile.logical_node_id;
    });
    if (profile.logical_node_id == 0 || node == graph.nodes.end() ||
        node->node_kind != profile.logical_node_kind ||
        profile.implementation_id.empty() || profile.capability_uuid.empty() ||
        profile.memory_bytes_required == 0) {
      result.diagnostic_id = "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1";
      result.detail = std::string(operation_name) +
                      " live node profile is incomplete or inconsistent";
      return result;
    }
    covered_nodes.insert(profile.logical_node_id);
    const auto [capability_index, inserted] = capability_indexes.emplace(
        profile.capability_uuid,
        executor_availability.capability_catalog.capabilities.size());
    if (inserted) {
      opt::CanonicalExecutorCapabilityRecord capability;
      capability.capability_uuid = profile.capability_uuid;
      capability.capability_abi_version = 1;
      capability.implementation_id = profile.implementation_id;
      capability.logical_node_kind = profile.logical_node_kind;
      capability.physical_node_kind = profile.physical_node_kind;
      capability.minimum_input_count = profile.minimum_input_count;
      capability.maximum_input_count = profile.maximum_input_count;
      capability.supported_property_kinds = profile.supported_property_kinds;
      capability.maximum_memory_bytes = std::max(
          request.optimizer_request.resource.memory_budget_bytes,
          profile.memory_bytes_required);
      capability.spill_supported = profile.spill_supported;
      capability.storage_read_capable = profile.storage_read_capable;
      capability.mga_visibility_capable = profile.mga_visibility_capable;
      capability.available = true;
      capability.engine_owned = true;
      executor_availability.capability_catalog.capabilities.push_back(
          std::move(capability));
    } else {
      auto& capability = executor_availability.capability_catalog.capabilities
          [capability_index->second];
      if (capability.implementation_id != profile.implementation_id ||
          capability.logical_node_kind != profile.logical_node_kind ||
          capability.physical_node_kind != profile.physical_node_kind ||
          capability.minimum_input_count != profile.minimum_input_count ||
          capability.maximum_input_count != profile.maximum_input_count ||
          capability.spill_supported != profile.spill_supported ||
          capability.storage_read_capable != profile.storage_read_capable ||
          capability.mga_visibility_capable !=
              profile.mga_visibility_capable) {
        result.diagnostic_id =
            "QOW-DIAG-OPTIMIZER-PROFILE-FACTORY-CAPABILITY-V1";
        result.detail = std::string(operation_name) +
                        " executor capability identity is inconsistent";
        return result;
      }
      capability.maximum_memory_bytes = std::max(
          capability.maximum_memory_bytes, profile.memory_bytes_required);
      for (const auto kind : profile.supported_property_kinds) {
        if (std::ranges::find(capability.supported_property_kinds, kind) ==
            capability.supported_property_kinds.end()) {
          capability.supported_property_kinds.push_back(kind);
        }
      }
    }
    opt::CanonicalOptimizerNodeCapabilityBinding binding;
    binding.logical_node_id = profile.logical_node_id;
    binding.capability_uuid = profile.capability_uuid;
    binding.memory_bytes_required = profile.memory_bytes_required;
    executor_availability.node_bindings.push_back(std::move(binding));
  }

  if (covered_nodes.size() != graph.nodes.size()) {
    result.diagnostic_id = "QOW-DIAG-OPTIMIZER-SEARCH-NO-PLAN-V1";
    result.detail = std::string(operation_name) +
                    " live profile does not cover every logical node";
    return result;
  }
  opt::RelationalDagPlanningInput planning_input;
  planning_input.admission_request = request.optimizer_request;
  planning_input.admission = request.optimizer_admission;
  planning_input.executor_availability = std::move(executor_availability);
  planning_input.identity_scope = identity_scope;
  planning_input.calibration_profile_uuid = calibration_uuid;
  planning_input.search_policy.maximum_exhaustive_plan_count = 1;
  planning_input.search_policy.bounded_beam_width = 1;
  planning_input.search_policy.deterministic_step_cost_ns = 1;
  planning_input.search_policy.engine_owned = true;
  planning_input.search_policy.allow_timeout_degradation = true;
  planning_input.publication_identity.selected_plan_uuid =
      DerivedCanonicalUuid(identity_scope, selected_plan_purpose);
  planning_input.publication_identity.first_causal_counter_id = 1;
  planning_input.publication_identity.engine_owned = true;
  auto planning = opt::PlanCanonicalRelationalDag(planning_input);
  if (!planning.accepted || !planning.optimizer_owned ||
      !planning.complete_logical_dag_covered ||
      !planning.physical_dag_published || planning.data_access_allowed ||
      !planning.diagnostics.empty()) {
    result.diagnostic_id = planning.diagnostics.empty()
                               ? "QOW-DIAG-RELATIONAL-DAG-PLANNING-V1"
                               : planning.diagnostics.front();
    result.detail = std::string(operation_name) +
                    " canonical relational-DAG planning refused the live implementation catalog";
    return result;
  }
  result.ok = true;
  result.physical_dag = std::move(planning.publication.physical_dag);
  for (const auto& physical_node : result.physical_dag.nodes) {
    const auto profile = std::ranges::find_if(
        profiles, [&](const LivePhysicalNodeProfile& candidate) {
          return candidate.logical_node_id ==
                     physical_node.relational_node_id &&
                 candidate.implementation_id ==
                     physical_node.implementation_id;
        });
    if (profile == profiles.end() || profile->memory_bytes_required == 0 ||
        !profile->runtime_producer_peak_memory_exact ||
        profile->runtime_producer_peak_memory_bytes == 0) {
      result.ok = false;
      result.physical_dag = {};
      result.ordinary_runtime_memory_receipts.clear();
      result.diagnostic_id = "QOW-DIAG-OPT-017-REFUSAL-V1";
      result.detail =
          "selected node lacks its separately carried producer memory receipt";
      return result;
    }
    OrdinaryRuntimeMemoryReceipt receipt;
    receipt.physical_node_id = physical_node.physical_node_id;
    receipt.implementation_id = physical_node.implementation_id;
    receipt.producer_peak_memory_bytes =
        profile->runtime_producer_peak_memory_bytes;
    receipt.producer_peak_memory_exact =
        profile->runtime_producer_peak_memory_exact;
    receipt.producer_peak_from_runtime_batches =
        profile->runtime_peak_from_callback_batches;
    receipt.accounted_auxiliary_memory_bytes =
        profile->runtime_accounted_auxiliary_memory_bytes;
    receipt.accounted_auxiliary_from_first_input_batch =
        profile->runtime_auxiliary_from_first_input_batch;
    const auto logical_node = std::ranges::find_if(
        graph.nodes, [&](const auto& candidate) {
          return candidate.logical_node_id == profile->logical_node_id;
        });
    receipt.residual_recheck_applicable =
        profile->physical_node_kind == exec::PhysicalNodeKind::kFilter ||
        (profile->physical_node_kind == exec::PhysicalNodeKind::kJoin &&
         logical_node != graph.nodes.end() &&
         logical_node->semantic_variant_id != "join.cross.v1");
    receipt.non_accessing_proven = !profile->storage_read_capable;
    if (!result.ordinary_runtime_memory_receipts
             .emplace(receipt.physical_node_id, std::move(receipt))
             .second) {
      result.ok = false;
      result.physical_dag = {};
      result.ordinary_runtime_memory_receipts.clear();
      result.diagnostic_id = "QOW-DIAG-OPT-017-REFUSAL-V1";
      result.detail = "producer memory receipt identity is not unique";
      return result;
    }
  }
  return result;
}

}  // namespace scratchbird::engine::sblr
