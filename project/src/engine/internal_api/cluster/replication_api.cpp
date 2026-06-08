// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "cluster/replication_api.hpp"

#include "api_diagnostics.hpp"
#include "cluster/cluster_provider_boundary.hpp"

#include <string>

namespace scratchbird::engine::internal_api {
namespace {

bool Empty(const EngineUuid& uuid) {
  return uuid.canonical.empty();
}

bool Empty(const EngineObjectReference& object) {
  return object.uuid.canonical.empty();
}

bool IsKnownBoundaryKind(const std::string& kind) {
  return kind == "publication" ||
         kind == "subscription" ||
         kind == "slot" ||
         kind == "cdc_changefeed" ||
         kind == "live_ingest";
}

EngineReplicationBoundaryResult BoundaryFailure(const EngineReplicationBoundaryRequest& request,
                                                std::string reason,
                                                std::string detail = {}) {
  EngineReplicationBoundaryResult result;
  result.ok = false;
  result.operation_id = "replication.evaluate_boundary";
  result.boundary_checked = true;
  result.fail_closed = true;
  result.refusal_reason = reason;
  result.route_epoch = request.route_epoch;
  result.route_generation = request.route_generation;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.cluster_authority_required = !request.context.cluster_authority_available;
  result.diagnostics.push_back(MakeInvalidRequestDiagnostic(result.operation_id,
                                                            detail.empty() ? reason : detail));
  result.evidence.push_back({"replication_boundary_refused", reason});
  result.evidence.push_back({"boundary_kind", request.boundary_kind});
  if (!request.context.cluster_authority_available) {
    result.standalone_fail_closed = true;
    result.evidence.push_back({"standalone_cluster_boundary", "replication_route_not_entered"});
  }
  return result;
}

void MarkBoundaryCoverage(EngineReplicationBoundaryResult* result, const std::string& kind) {
  if (result == nullptr) {
    return;
  }
  result->publication_checked = kind == "publication" || kind == "cdc_changefeed" ||
                                kind == "live_ingest";
  result->subscription_checked = kind == "subscription" || kind == "cdc_changefeed" ||
                                 kind == "live_ingest";
  result->slot_checked = kind == "slot" || kind == "cdc_changefeed";
  result->changefeed_checked = kind == "cdc_changefeed";
  result->live_ingest_checked = kind == "live_ingest";
}

EngineInspectReplicationResult ReplicationInspectProviderBoundaryFailure(
    const EngineApiResult& provider_result) {
  constexpr std::string_view kOperation = "cluster.inspect_replication";
  EngineInspectReplicationResult result;
  result.replication_boundary_present = true;
  result.standalone_fail_closed = true;
  (void)RefuseIfClusterProviderBoundaryClosed(&result,
                                              provider_result,
                                              kOperation,
                                              "cluster_provider_boundary_closed");
  result.evidence.push_back({"replication_inspect_not_entered",
                             "provider_boundary_failed_closed"});
  return result;
}

EngineReplicationBoundaryResult ReplicationProviderBoundaryFailure(
    const EngineReplicationBoundaryRequest& request,
    const EngineApiResult& provider_result) {
  constexpr std::string_view kOperation = "replication.evaluate_boundary";
  EngineReplicationBoundaryResult result;
  result.boundary_checked = true;
  result.security_checked = true;
  result.retention_checked = request.boundary_kind == "cdc_changefeed";
  result.fail_closed = true;
  result.standalone_fail_closed = true;
  result.refusal_reason = "cluster_provider_boundary_closed";
  result.route_epoch = request.route_epoch;
  result.route_generation = request.route_generation;
  result.primary_object = request.target_object;
  result.transaction_uuid = request.context.transaction_uuid;
  result.local_transaction_id = request.context.local_transaction_id;
  MarkBoundaryCoverage(&result, request.boundary_kind);
  (void)RefuseIfClusterProviderBoundaryClosed(&result,
                                              provider_result,
                                              kOperation,
                                              result.refusal_reason);
  result.evidence.push_back({"replication_route_not_entered",
                             "provider_boundary_failed_closed"});
  result.evidence.push_back({"replication_boundary_kind", request.boundary_kind});
  return result;
}

}  // namespace

EngineInspectReplicationResult EngineInspectReplication(const EngineInspectReplicationRequest& request) {
  EngineInspectReplicationResult result;
  result.operation_id = "cluster.inspect_replication";
  result.replication_boundary_present = true;
  result.cluster_authority_required = true;
  result.standalone_fail_closed = !request.context.cluster_authority_available;
  result.ok = request.context.cluster_authority_available;
  if (!result.ok) {
    result.diagnostics.push_back(MakeClusterAuthorityUnavailableDiagnostic(result.operation_id));
    result.evidence.push_back({"standalone_cluster_boundary", "replication_inspect_not_entered"});
    result.evidence.push_back({"cluster_authority", "unavailable"});
    return result;
  }
  const auto provider_result = ExecuteInternalClusterProviderBoundary(
      request.context,
      "cluster.inspect_replication",
      "SBLR_CLUSTER_INSPECT_REPLICATION",
      "internal-api.cluster-inspect-replication");
  if (!provider_result.ok) {
    return ReplicationInspectProviderBoundaryFailure(provider_result);
  }
  result.evidence.push_back({"replication_boundary", "inspect_authorized"});
  CopyClusterProviderBoundaryProof(&result,
                                   provider_result,
                                   "cluster.inspect_replication");
  result.evidence.push_back({"cluster_mapping", "not_activated_by_inspect"});
  return result;
}

EngineReplicationBoundaryResult EngineEvaluateReplicationBoundary(
    const EngineReplicationBoundaryRequest& request) {
  if (!request.engine_authoritative) {
    return BoundaryFailure(request, "engine_authority_required");
  }
  if (!request.context.security_context_present || !request.security_authorized) {
    auto result = BoundaryFailure(request, "security_authorization_required");
    result.security_checked = true;
    return result;
  }
  if (!request.context.cluster_authority_available) {
    auto result = BoundaryFailure(request, "cluster_authority_unavailable");
    result.cluster_authority_required = true;
    result.standalone_fail_closed = true;
    return result;
  }
  if (request.context.local_transaction_id == 0 || Empty(request.context.transaction_uuid)) {
    return BoundaryFailure(request, "transaction_context_required");
  }
  if (!IsKnownBoundaryKind(request.boundary_kind)) {
    return BoundaryFailure(request, "replication_boundary_kind_unknown");
  }
  if (Empty(request.target_object)) {
    return BoundaryFailure(request, "target_object_uuid_required");
  }
  if ((request.boundary_kind == "publication" || request.boundary_kind == "cdc_changefeed" ||
       request.boundary_kind == "live_ingest") &&
      Empty(request.publication)) {
    return BoundaryFailure(request, "publication_uuid_required");
  }
  if ((request.boundary_kind == "subscription" || request.boundary_kind == "cdc_changefeed" ||
       request.boundary_kind == "live_ingest") &&
      Empty(request.subscription)) {
    return BoundaryFailure(request, "subscription_uuid_required");
  }
  if ((request.boundary_kind == "slot" || request.boundary_kind == "cdc_changefeed") &&
      Empty(request.slot)) {
    return BoundaryFailure(request, "replication_slot_uuid_required");
  }
  if (request.boundary_kind == "cdc_changefeed" &&
      request.retention_horizon_local_transaction_id == 0) {
    return BoundaryFailure(request, "retention_horizon_required");
  }
  if (request.boundary_kind == "cdc_changefeed" &&
      !request.backup_archive_hold_satisfied) {
    return BoundaryFailure(request, "backup_archive_hold_required");
  }
  if ((request.boundary_kind == "subscription" || request.boundary_kind == "cdc_changefeed" ||
       request.boundary_kind == "live_ingest") &&
      !request.event_channel_authorized) {
    return BoundaryFailure(request, "event_channel_authorization_required");
  }
  if (request.live_ingest_requested && !request.capability_profile_allows) {
    return BoundaryFailure(request, "live_ingest_capability_required");
  }
  if (request.route_epoch == 0 || request.route_generation == 0 ||
      request.policy_snapshot_uuid.empty() || request.idempotency_key.empty()) {
    return BoundaryFailure(request, "route_epoch_policy_and_idempotency_required");
  }
  const auto provider_result = ExecuteInternalClusterProviderBoundary(
      request.context,
      "cluster.inspect_replication",
      "SBLR_CLUSTER_INSPECT_REPLICATION",
      "internal-api.replication-boundary");
  if (!provider_result.ok) {
    return ReplicationProviderBoundaryFailure(request, provider_result);
  }

  EngineReplicationBoundaryResult result;
  result.ok = true;
  result.operation_id = "replication.evaluate_boundary";
  result.boundary_checked = true;
  result.security_checked = true;
  result.retention_checked = request.boundary_kind == "cdc_changefeed";
  result.route_activation_allowed = false;
  result.fail_closed = true;
  result.refusal_reason = "replication_mapping_unavailable";
  result.route_epoch = request.route_epoch;
  result.route_generation = request.route_generation;
  result.primary_object = request.target_object;
  result.transaction_uuid = request.context.transaction_uuid;
  result.local_transaction_id = request.context.local_transaction_id;
  MarkBoundaryCoverage(&result, request.boundary_kind);
  result.evidence.push_back({"replication_boundary", "validated"});
  CopyClusterProviderBoundaryProof(&result,
                                   provider_result,
                                   "cluster.inspect_replication");
  result.evidence.push_back({"replication_boundary_kind", request.boundary_kind});
  result.evidence.push_back({"replication_route_activation", "fail_closed_until_mapping"});
  result.evidence.push_back({"policy_snapshot_uuid", request.policy_snapshot_uuid});
  result.evidence.push_back({"route_epoch", std::to_string(request.route_epoch)});
  result.evidence.push_back({"route_generation", std::to_string(request.route_generation)});
  if (!request.publication.uuid.canonical.empty()) {
    result.evidence.push_back({"publication_uuid", request.publication.uuid.canonical});
  }
  if (!request.subscription.uuid.canonical.empty()) {
    result.evidence.push_back({"subscription_uuid", request.subscription.uuid.canonical});
  }
  if (!request.slot.uuid.canonical.empty()) {
    result.evidence.push_back({"replication_slot_uuid", request.slot.uuid.canonical});
  }
  return result;
}

}  // namespace scratchbird::engine::internal_api
