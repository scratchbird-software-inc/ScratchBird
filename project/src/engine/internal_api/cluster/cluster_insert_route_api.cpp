// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SPDX-License-Identifier: Apache-2.0
// Copyright (c) ScratchBird contributors
//
// SB_ENGINE_INTERNAL_API_CLUSTER_INSERT_ROUTE_FENCE_BEHAVIOR
// SB_PID012_CLUSTER_INSERT_ROUTE_FENCE

#include "cluster/cluster_insert_route_api.hpp"

#include "api_diagnostics.hpp"
#include "cluster/cluster_provider_boundary.hpp"
#include "metric_contracts.hpp"

namespace scratchbird::engine::internal_api {
namespace {

bool Empty(const EngineUuid& uuid) {
  return uuid.canonical.empty();
}

bool Empty(const EngineObjectReference& object) {
  return object.uuid.canonical.empty();
}

std::string MetricLabel(const std::string& value) {
  return value.empty() ? "unknown" : value;
}

void RecordRouteFenceMetrics(const EngineClusterInsertRouteFenceRequest& request,
                             const std::string& result,
                             const std::string& reason) {
  if (!request.context.cluster_authority_available) {
    return;
  }
  const std::string database_uuid = MetricLabel(request.context.database_uuid.canonical);
  const std::string table_uuid = MetricLabel(request.target_table.uuid.canonical);
  const std::string route_epoch = request.route_epoch == 0 ? "unknown" : std::to_string(request.route_epoch);
  (void)scratchbird::core::metrics::RecordClusterInsertRouteCheck(database_uuid,
                                                                  table_uuid,
                                                                  route_epoch,
                                                                  result,
                                                                  reason);
  (void)scratchbird::core::metrics::RecordClusterInsertFailClosed(database_uuid,
                                                                 table_uuid,
                                                                 "route_fence",
                                                                 reason);
  if (reason == "stale_owner_refused") {
    (void)scratchbird::core::metrics::RecordClusterInsertStaleRouteRejection(
        database_uuid,
        table_uuid,
        route_epoch,
        MetricLabel(request.owner_node_uuid.canonical));
  }
}

EngineClusterInsertRouteFenceResult RouteFenceFailure(const EngineClusterInsertRouteFenceRequest& request,
                                                      const std::string& reason) {
  EngineClusterInsertRouteFenceResult result;
  result.ok = false;
  result.operation_id = "cluster.validate_insert_route_fence";
  result.route_fence_checked = true;
  result.fail_closed = true;
  result.refusal_reason = reason;
  result.route_epoch = request.route_epoch;
  result.route_generation = request.route_generation;
  result.cluster_authority_required = true;
  result.diagnostics.push_back(MakeInvalidRequestDiagnostic("cluster.validate_insert_route_fence", reason));
  result.evidence.push_back({"cluster_insert_route_refused", reason});
  if (!request.context.cluster_authority_available) {
    result.evidence.push_back({"standalone_cluster_boundary", "cluster_metric_path_skipped"});
    result.evidence.push_back({"standalone_cluster_boundary", "cluster_route_not_entered"});
  }
  if (!request.target_table.uuid.canonical.empty()) {
    result.evidence.push_back({"target_table_uuid", request.target_table.uuid.canonical});
  }
  RecordRouteFenceMetrics(request, "refused", reason);
  return result;
}

EngineClusterInsertRouteFenceResult ProviderBoundaryFailure(
    const EngineClusterInsertRouteFenceRequest& request,
    const EngineApiResult& provider_result) {
  constexpr std::string_view kOperation = "cluster.validate_insert_route_fence";
  EngineClusterInsertRouteFenceResult result;
  result.route_fence_checked = false;
  result.fail_closed = true;
  result.refusal_reason = "cluster_provider_boundary_closed";
  result.route_epoch = request.route_epoch;
  result.route_generation = request.route_generation;
  result.primary_object = request.target_table;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  (void)RefuseIfClusterProviderBoundaryClosed(&result,
                                              provider_result,
                                              kOperation,
                                              result.refusal_reason);
  result.evidence.push_back({"cluster_insert_route_not_entered",
                             "provider_boundary_failed_closed"});
  return result;
}

}  // namespace

EngineClusterInsertRouteFenceResult EngineValidateClusterInsertRouteFence(
    const EngineClusterInsertRouteFenceRequest& request) {
  if (!request.context.cluster_authority_available) {
    return RouteFenceFailure(request, "cluster_authority_unavailable");
  }
  const auto provider_result = ExecuteInternalClusterProviderBoundary(
      request.context,
      "cluster.validate_insert_route_fence",
      "SBLR_CLUSTER_VALIDATE_INSERT_ROUTE_FENCE",
      "internal-api.cluster-insert-route-fence");
  if (!provider_result.ok) {
    return ProviderBoundaryFailure(request, provider_result);
  }
  if (request.context.local_transaction_id == 0) {
    return RouteFenceFailure(request, "local_transaction_id_required");
  }
  if (!request.localized_names.empty()) {
    return RouteFenceFailure(request, "localized_names_not_allowed_engine_boundary");
  }
  if (Empty(request.target_table)) {
    return RouteFenceFailure(request, "target_table_uuid_required");
  }
  if (Empty(request.target_shard)) {
    return RouteFenceFailure(request, "target_shard_uuid_required");
  }
  if (Empty(request.target_range)) {
    return RouteFenceFailure(request, "target_range_uuid_required");
  }
  if (Empty(request.owner_node_uuid)) {
    return RouteFenceFailure(request, "owner_node_uuid_required");
  }
  if (request.remote_participant_requested && Empty(request.participant_node_uuid)) {
    return RouteFenceFailure(request, "participant_node_uuid_required");
  }
  if (Empty(request.route_epoch_uuid) || request.route_epoch == 0 || request.route_generation == 0) {
    return RouteFenceFailure(request, "route_epoch_and_generation_required");
  }
  if (Empty(request.participant_uuid)) {
    return RouteFenceFailure(request, "participant_uuid_required");
  }
  if (Empty(request.policy_snapshot_uuid)) {
    return RouteFenceFailure(request, "policy_snapshot_uuid_required");
  }
  if (Empty(request.finality_service_uuid)) {
    return RouteFenceFailure(request, "finality_service_uuid_required");
  }
  if (request.idempotency_key.empty()) {
    return RouteFenceFailure(request, "idempotency_key_required");
  }
  if (request.stale_owner_observed) {
    auto result = RouteFenceFailure(request, "stale_owner_refused");
    result.stale_owner_refused = true;
    return result;
  }
  if (request.unresolved_prepared_or_limbo_work && request.handoff_proof_ref.empty()) {
    auto result = RouteFenceFailure(request, "handoff_proof_required_for_unresolved_prepared_or_limbo_work");
    result.handoff_proof_required = true;
    return result;
  }

  EngineClusterInsertRouteFenceResult result;
  result.ok = true;
  result.operation_id = "cluster.validate_insert_route_fence";
  result.route_fence_checked = true;
  result.route_activation_allowed = false;
  result.remote_participant_allowed = false;
  result.fail_closed = true;
  result.finality_required = true;
  result.route_epoch = request.route_epoch;
  result.route_generation = request.route_generation;
  result.refusal_reason = "cluster_mapping_unavailable";
  result.primary_object = request.target_table;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.evidence.push_back({"cluster_insert_route_contract", "validated"});
  CopyClusterProviderBoundaryProof(&result,
                                   provider_result,
                                   "cluster.validate_insert_route_fence");
  result.evidence.push_back({"cluster_insert_route_activation", "fail_closed_until_finality"});
  result.evidence.push_back({"target_table_uuid", request.target_table.uuid.canonical});
  result.evidence.push_back({"target_shard_uuid", request.target_shard.uuid.canonical});
  result.evidence.push_back({"target_range_uuid", request.target_range.uuid.canonical});
  result.evidence.push_back({"owner_node_uuid", request.owner_node_uuid.canonical});
  result.evidence.push_back({"route_epoch_uuid", request.route_epoch_uuid.canonical});
  result.evidence.push_back({"route_epoch", std::to_string(request.route_epoch)});
  result.evidence.push_back({"route_generation", std::to_string(request.route_generation)});
  result.evidence.push_back({"participant_uuid", request.participant_uuid.canonical});
  result.evidence.push_back({"policy_snapshot_uuid", request.policy_snapshot_uuid.canonical});
  result.evidence.push_back({"finality_service_uuid", request.finality_service_uuid.canonical});
  if (!request.participant_node_uuid.canonical.empty()) {
    result.evidence.push_back({"participant_node_uuid", request.participant_node_uuid.canonical});
  }
  if (!request.handoff_proof_ref.empty()) {
    result.evidence.push_back({"handoff_proof_ref", request.handoff_proof_ref});
  }
  RecordRouteFenceMetrics(request, "validated_fail_closed", result.refusal_reason);
  return result;
}

}  // namespace scratchbird::engine::internal_api
