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
// SB_ENGINE_INTERNAL_API_REMOTE_PARTICIPANT_INSERT_API_BEHAVIOR
// SB_PID013_REMOTE_PARTICIPANT_INSERT_API

#include "cluster/remote_participant_insert_api.hpp"

#include <initializer_list>
#include <string>

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

bool OneOf(const std::string& value, std::initializer_list<const char*> allowed) {
  for (const char* item : allowed) {
    if (value == item) {
      return true;
    }
  }
  return false;
}

std::string MetricLabel(const std::string& value) {
  return value.empty() ? "unknown" : value;
}

void RecordParticipantMetrics(const EngineRemoteParticipantInsertRequest& request,
                              const std::string& result,
                              const std::string& reason,
                              bool retryable) {
  if (!request.context.cluster_authority_available) {
    return;
  }
  const std::string database_uuid = MetricLabel(request.target_database.uuid.canonical);
  const std::string table_uuid = MetricLabel(request.target_table.uuid.canonical);
  const std::string participant_node_uuid = MetricLabel(request.participant_node_uuid.canonical);
  (void)scratchbird::core::metrics::RecordClusterInsertParticipantAdmission(database_uuid,
                                                                           table_uuid,
                                                                           participant_node_uuid,
                                                                           result,
                                                                           reason);
  (void)scratchbird::core::metrics::RecordClusterInsertRemoteRequest(database_uuid,
                                                                    table_uuid,
                                                                    participant_node_uuid,
                                                                    result,
                                                                    retryable ? "retryable" : "not_retryable");
  (void)scratchbird::core::metrics::RecordClusterInsertFailClosed(database_uuid,
                                                                 table_uuid,
                                                                 "remote_participant",
                                                                 reason);
}

EngineRemoteParticipantInsertResult ParticipantFailure(const EngineRemoteParticipantInsertRequest& request,
                                                       const std::string& reason,
                                                       bool retryable) {
  EngineRemoteParticipantInsertResult result;
  result.ok = false;
  result.operation_id = "cluster.prepare_remote_participant_insert";
  result.participant_envelope_validated = true;
  result.fail_closed = true;
  result.retryable = retryable;
  result.route_epoch = request.route_epoch;
  result.route_generation = request.route_generation;
  result.canonical_row_count = static_cast<EngineApiU64>(request.canonical_rows.size());
  result.normalized_retry_policy = request.retry_policy;
  result.normalized_failure_model = request.failure_model;
  result.normalized_insert_mode = request.insert_mode;
  result.refusal_reason = reason;
  result.cluster_authority_required = true;
  result.diagnostics.push_back(MakeInvalidRequestDiagnostic("cluster.prepare_remote_participant_insert", reason));
  result.evidence.push_back({"remote_participant_insert_refused", reason});
  if (!request.context.cluster_authority_available) {
    result.evidence.push_back({"standalone_cluster_boundary", "cluster_metric_path_skipped"});
    result.evidence.push_back({"standalone_cluster_boundary", "remote_participant_path_not_entered"});
  }
  if (!request.remote_request_uuid.canonical.empty()) {
    result.evidence.push_back({"remote_request_uuid", request.remote_request_uuid.canonical});
  }
  if (!request.target_table.uuid.canonical.empty()) {
    result.evidence.push_back({"target_table_uuid", request.target_table.uuid.canonical});
  }
  RecordParticipantMetrics(request, "refused", reason, retryable);
  return result;
}

EngineRemoteParticipantInsertResult ProviderBoundaryFailure(
    const EngineRemoteParticipantInsertRequest& request,
    const EngineApiResult& provider_result) {
  constexpr std::string_view kOperation = "cluster.prepare_remote_participant_insert";
  EngineRemoteParticipantInsertResult result;
  result.participant_envelope_validated = false;
  result.fail_closed = true;
  result.retryable = true;
  result.route_epoch = request.route_epoch;
  result.route_generation = request.route_generation;
  result.canonical_row_count = static_cast<EngineApiU64>(request.canonical_rows.size());
  result.normalized_retry_policy = request.retry_policy;
  result.normalized_failure_model = request.failure_model;
  result.normalized_insert_mode = request.insert_mode;
  result.refusal_reason = "cluster_provider_boundary_closed";
  result.primary_object = request.target_table;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  (void)RefuseIfClusterProviderBoundaryClosed(&result,
                                              provider_result,
                                              kOperation,
                                              result.refusal_reason);
  result.evidence.push_back({"remote_participant_path_not_entered",
                             "provider_boundary_failed_closed"});
  return result;
}

}  // namespace

EngineRemoteParticipantInsertResult EnginePrepareRemoteParticipantInsert(
    const EngineRemoteParticipantInsertRequest& request) {
  if (!request.context.cluster_authority_available) {
    return ParticipantFailure(request, "cluster_authority_unavailable", true);
  }
  const auto provider_result = ExecuteInternalClusterProviderBoundary(
      request.context,
      "cluster.prepare_remote_participant_insert",
      "SBLR_CLUSTER_PREPARE_REMOTE_PARTICIPANT_INSERT",
      "internal-api.cluster-remote-participant-insert");
  if (!provider_result.ok) {
    return ProviderBoundaryFailure(request, provider_result);
  }
  if (request.context.local_transaction_id == 0) {
    return ParticipantFailure(request, "local_transaction_id_required", false);
  }
  if (!request.localized_names.empty()) {
    return ParticipantFailure(request, "localized_names_not_allowed_engine_boundary", false);
  }
  if (Empty(request.remote_request_uuid)) {
    return ParticipantFailure(request, "remote_request_uuid_required", false);
  }
  if (Empty(request.target_database)) {
    return ParticipantFailure(request, "target_database_uuid_required", false);
  }
  if (Empty(request.target_table)) {
    return ParticipantFailure(request, "target_table_uuid_required", false);
  }
  if (Empty(request.target_shard)) {
    return ParticipantFailure(request, "target_shard_uuid_required", false);
  }
  if (Empty(request.target_range)) {
    return ParticipantFailure(request, "target_range_uuid_required", false);
  }
  if (Empty(request.owner_node_uuid)) {
    return ParticipantFailure(request, "owner_node_uuid_required", false);
  }
  if (Empty(request.participant_node_uuid)) {
    return ParticipantFailure(request, "participant_node_uuid_required", false);
  }
  if (Empty(request.route_epoch_uuid) || request.route_epoch == 0 || request.route_generation == 0) {
    return ParticipantFailure(request, "route_epoch_and_generation_required", false);
  }
  if (Empty(request.participant_uuid)) {
    return ParticipantFailure(request, "participant_uuid_required", false);
  }
  if (Empty(request.policy_snapshot_uuid)) {
    return ParticipantFailure(request, "policy_snapshot_uuid_required", false);
  }
  if (Empty(request.finality_service_uuid)) {
    return ParticipantFailure(request, "finality_service_uuid_required", true);
  }
  if (request.idempotency_key.empty()) {
    return ParticipantFailure(request, "idempotency_key_required", false);
  }
  if (!request.route_fence_validated) {
    return ParticipantFailure(request, "route_fence_validation_required", false);
  }
  if (request.canonical_rows.empty()) {
    return ParticipantFailure(request, "canonical_rows_required", false);
  }
  if (!OneOf(request.retry_policy, {"fail_closed", "retry_idempotent", "operator_review_required"})) {
    return ParticipantFailure(request, "retry_policy_unsupported:" + request.retry_policy, false);
  }
  if (!OneOf(request.failure_model,
             {"participant_refusal", "retryable_transport_failure", "finality_unknown", "operator_review_required"})) {
    return ParticipantFailure(request, "failure_model_unsupported:" + request.failure_model, false);
  }
  if (!OneOf(request.insert_mode, {"copy_import", "donor_bulk", "native_bulk", "multi_values"})) {
    return ParticipantFailure(request, "insert_mode_unsupported:" + request.insert_mode, false);
  }
  if (request.import_execution_requested && request.source_fingerprint.empty()) {
    return ParticipantFailure(request, "import_source_fingerprint_required", false);
  }
  if (request.import_execution_requested && request.source_position.empty()) {
    return ParticipantFailure(request, "import_source_position_required", false);
  }
  if (!Empty(request.checkpoint_uuid) && !request.import_execution_requested) {
    return ParticipantFailure(request, "checkpoint_uuid_requires_import_execution", false);
  }
  if (!request.participant_admission_durable) {
    return ParticipantFailure(request, "durable_participant_admission_required", false);
  }
  if (!request.finality_proof_available) {
    EngineRemoteParticipantInsertResult result = ParticipantFailure(request,
                                                                    "cluster_mapping_unavailable",
                                                                    request.retry_policy == "retry_idempotent");
    result.participant_admitted = true;
    result.finality_required = true;
    return result;
  }

  EngineRemoteParticipantInsertResult result;
  result.ok = true;
  result.operation_id = "cluster.prepare_remote_participant_insert";
  result.participant_envelope_validated = true;
  result.participant_admitted = true;
  result.local_insert_allowed = false;
  result.remote_execution_allowed = false;
  result.finality_required = true;
  result.fail_closed = true;
  result.retryable = false;
  result.checkpoint_replay_required = !Empty(request.checkpoint_uuid);
  result.canonical_row_count = static_cast<EngineApiU64>(request.canonical_rows.size());
  result.route_epoch = request.route_epoch;
  result.route_generation = request.route_generation;
  result.normalized_retry_policy = request.retry_policy;
  result.normalized_failure_model = request.failure_model;
  result.normalized_insert_mode = request.insert_mode;
  result.refusal_reason = "remote_execution_disabled_until_cluster_finality_executor_exists";
  result.primary_object = request.target_table;
  result.local_transaction_id = request.context.local_transaction_id;
  result.transaction_uuid = request.context.transaction_uuid;
  result.evidence.push_back({"remote_participant_insert_contract", "validated"});
  CopyClusterProviderBoundaryProof(&result,
                                   provider_result,
                                   "cluster.prepare_remote_participant_insert");
  result.evidence.push_back({"remote_execution", "fail_closed_until_cluster_finality_executor_exists"});
  result.evidence.push_back({"remote_request_uuid", request.remote_request_uuid.canonical});
  result.evidence.push_back({"target_database_uuid", request.target_database.uuid.canonical});
  result.evidence.push_back({"target_table_uuid", request.target_table.uuid.canonical});
  result.evidence.push_back({"target_shard_uuid", request.target_shard.uuid.canonical});
  result.evidence.push_back({"target_range_uuid", request.target_range.uuid.canonical});
  result.evidence.push_back({"owner_node_uuid", request.owner_node_uuid.canonical});
  result.evidence.push_back({"participant_node_uuid", request.participant_node_uuid.canonical});
  result.evidence.push_back({"route_epoch_uuid", request.route_epoch_uuid.canonical});
  result.evidence.push_back({"route_epoch", std::to_string(request.route_epoch)});
  result.evidence.push_back({"route_generation", std::to_string(request.route_generation)});
  result.evidence.push_back({"participant_uuid", request.participant_uuid.canonical});
  result.evidence.push_back({"policy_snapshot_uuid", request.policy_snapshot_uuid.canonical});
  result.evidence.push_back({"finality_service_uuid", request.finality_service_uuid.canonical});
  result.evidence.push_back({"canonical_row_count", std::to_string(result.canonical_row_count)});
  if (!request.checkpoint_uuid.canonical.empty()) {
    result.evidence.push_back({"checkpoint_uuid", request.checkpoint_uuid.canonical});
  }
  RecordParticipantMetrics(request, "validated_fail_closed", result.refusal_reason, result.retryable);
  return result;
}

}  // namespace scratchbird::engine::internal_api
