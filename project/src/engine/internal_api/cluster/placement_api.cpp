// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster/placement_api.hpp"

#include "api_diagnostics.hpp"
#include "behavior_support/api_behavior_store.hpp"
#include "cluster/cluster_provider_boundary.hpp"

#include <algorithm>
#include <string>
#include <string_view>

namespace scratchbird::engine::internal_api {
namespace {

bool IsSupportedPlacementOperation(std::string_view operation) {
  static constexpr std::string_view kOperations[] = {
      "create", "verify", "move", "split", "merge", "rebalance",
      "freeze", "archive", "reattach", "quarantine", "reconcile", "drop"};
  return std::find(std::begin(kOperations), std::end(kOperations), operation) !=
         std::end(kOperations);
}

bool RequiresMutationContext(std::string_view operation) {
  return operation != "verify";
}

bool RequiresPhysicalMovement(std::string_view operation) {
  return operation == "move" || operation == "split" || operation == "merge" ||
         operation == "rebalance" || operation == "reattach";
}

std::string StateForOperation(std::string_view operation) {
  if (operation == "create") return "created_descriptor";
  if (operation == "verify") return "verified_descriptor";
  if (operation == "move") return "move_planned";
  if (operation == "split") return "split_planned";
  if (operation == "merge") return "merge_planned";
  if (operation == "rebalance") return "rebalance_planned";
  if (operation == "freeze") return "frozen";
  if (operation == "archive") return "archived";
  if (operation == "reattach") return "reattach_planned";
  if (operation == "quarantine") return "quarantined";
  if (operation == "reconcile") return "reconcile_planned";
  if (operation == "drop") return "drop_planned";
  return "unknown";
}

std::string RecommendedActionForOperation(std::string_view operation) {
  if (operation == "verify") return "inspect_descriptor_only";
  if (operation == "quarantine") return "fence_shard_and_require_operator_release";
  if (operation == "drop") return "operator_review_before_physical_drop";
  if (RequiresPhysicalMovement(operation)) return "plan_requires_physical_movement_workflow";
  return "operator_review_before_apply";
}

bool DescriptorHasRequiredIdentity(const EngineShardPlacementDescriptor& descriptor) {
  return !descriptor.shard_uuid.empty() && !descriptor.target_filespace_uuid.empty();
}

EngineShardPlacementOperationResult PlacementFailure(
    const EngineShardPlacementOperationRequest& request,
    const std::string& detail) {
  const std::string operation_id = request.operation_id.empty()
                                       ? "cluster.shard_placement.plan"
                                       : request.operation_id;
  return MakeApiBehaviorDiagnostic<EngineShardPlacementOperationResult>(
      request.context,
      operation_id,
      MakeInvalidRequestDiagnostic(operation_id, detail));
}

}  // namespace

// SEARCH_KEY: SB_ENGINE_INTERNAL_API_CLUSTER_PLACEMENT_API_STUBS
EnginePlaceClusterObjectResult EnginePlaceClusterObject(const EnginePlaceClusterObjectRequest& request) {
  constexpr std::string_view kOperation = "cluster.place_object";
  auto result = EnginePlaceClusterObjectResult{};
  const auto provider_result = ExecuteInternalClusterProviderBoundary(
      request.context,
      kOperation,
      "SBLR_CLUSTER_PLACE_OBJECT",
      "internal-api.cluster-place-object");
  if (RefuseIfClusterProviderBoundaryClosed(&result,
                                            provider_result,
                                            kOperation,
                                            "cluster_provider_boundary_closed")) {
    return result;
  }
  result.ok = false;
  result.operation_id = std::string(kOperation);
  result.cluster_authority_required = true;
  result.evidence.push_back({"cluster_place_object_refused",
                             "external_provider_result_not_mapped"});
  return result;
}

EngineShardPlacementOperationResult EnginePlanShardPlacementOperation(
    const EngineShardPlacementOperationRequest& request) {
  constexpr const char* kOperation = "cluster.shard_placement.plan";
  const std::string operation_id = request.operation_id.empty()
                                       ? std::string(kOperation)
                                       : request.operation_id;
  if (!request.context.security_context_present) {
    return PlacementFailure(request, "security_context_required");
  }
  if (request.placement_operation.empty()) {
    return PlacementFailure(request, "placement_operation_required");
  }
  if (!IsSupportedPlacementOperation(request.placement_operation)) {
    return PlacementFailure(request, "placement_operation_unsupported");
  }
  if (!DescriptorHasRequiredIdentity(request.descriptor)) {
    return PlacementFailure(request, "shard_and_target_filespace_required");
  }
  if (RequiresMutationContext(request.placement_operation) &&
      request.context.local_transaction_id == 0) {
    return PlacementFailure(request, "local_transaction_id_required");
  }
  if (request.placement_operation == "merge" && request.merge_inputs.size() < 2) {
    return PlacementFailure(request, "merge_requires_two_input_shards");
  }
  if ((request.placement_operation == "split" || request.placement_operation == "move") &&
      request.descriptor.source_filespace_uuid.empty()) {
    return PlacementFailure(request, "source_filespace_required");
  }
  if (request.physical_data_movement_requested) {
    return PlacementFailure(request, "physical_data_movement_not_available_in_open_core_descriptor_plan");
  }

  auto result = MakeApiBehaviorSuccess<EngineShardPlacementOperationResult>(
      request.context, operation_id);
  result.descriptor_validated = true;
  result.operation_verified = request.placement_operation == "verify";
  result.operation_planned = !result.operation_verified;
  result.operator_review_required = !result.operation_verified;
  result.physical_data_movement_required =
      RequiresPhysicalMovement(request.placement_operation);
  result.durable_state_changed = false;
  result.private_cluster_execution = false;
  result.cluster_provider_dispatch = false;
  result.result_shape.result_kind = "rs.shard_placement.descriptor_plan.v1";
  result.placement_state = StateForOperation(request.placement_operation);
  result.recommended_action = RecommendedActionForOperation(request.placement_operation);

  AddApiBehaviorRow(&result,
                    {{"placement_operation", request.placement_operation},
                     {"route_operation_id", operation_id},
                     {"shard_uuid", request.descriptor.shard_uuid},
                     {"source_filespace_uuid", request.descriptor.source_filespace_uuid},
                     {"target_filespace_uuid", request.descriptor.target_filespace_uuid},
                     {"placement_epoch", std::to_string(request.descriptor.placement_epoch)},
                     {"placement_generation",
                      std::to_string(request.descriptor.placement_generation)},
                     {"placement_state", result.placement_state},
                     {"operator_review_required",
                      result.operator_review_required ? "true" : "false"},
                     {"physical_data_movement_required",
                      result.physical_data_movement_required ? "true" : "false"},
                     {"durable_state_changed", "false"},
                     {"physical_data_movement_dispatched", "false"},
                     {"parser_storage_authority", "false"},
                     {"transaction_finality_authority", "false"},
                     {"recovery_authority", "false"},
                     {"reference_wal_recovery_authority", "false"},
                     {"private_cluster_execution", "false"},
                     {"cluster_provider_dispatch", "false"}});
  AddApiBehaviorEvidence(&result, "shard_placement_operation", request.placement_operation);
  AddApiBehaviorEvidence(&result, "shard_placement_state", result.placement_state);
  AddApiBehaviorEvidence(&result, "shard_uuid", request.descriptor.shard_uuid);
  AddApiBehaviorEvidence(&result, "target_filespace_uuid", request.descriptor.target_filespace_uuid);
  AddApiBehaviorEvidence(&result, "durable_state_changed", "false");
  AddApiBehaviorEvidence(&result, "physical_data_movement_dispatched", "false");
  AddApiBehaviorEvidence(&result, "parser_storage_authority", "false");
  AddApiBehaviorEvidence(&result, "transaction_finality_authority", "false");
  AddApiBehaviorEvidence(&result, "recovery_authority", "false");
  AddApiBehaviorEvidence(&result, "reference_wal_recovery_authority", "false");
  AddApiBehaviorEvidence(&result, "private_cluster_execution", "false");
  AddApiBehaviorEvidence(&result, "cluster_provider_dispatch", "false");
  AddApiBehaviorEvidence(&result, "mga_visibility_authority", "durable_transaction_inventory");
  result.result_shape.result_kind = "rs.shard_placement.descriptor_plan.v1";
  return result;
}

}  // namespace scratchbird::engine::internal_api
