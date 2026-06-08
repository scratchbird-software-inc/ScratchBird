// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "vector_maintenance_jobs.hpp"

#include <algorithm>
#include <utility>

namespace scratchbird::core::agents {
namespace {

namespace idx = scratchbird::core::index;

bool KnownAction(VectorMaintenanceActionKind action) {
  return action == VectorMaintenanceActionKind::adaptive_tuning ||
         action == VectorMaintenanceActionKind::retrain ||
         action == VectorMaintenanceActionKind::rebuild;
}

OnlineMaintenanceOperationKind OnlineKind(VectorMaintenanceActionKind action) {
  switch (action) {
    case VectorMaintenanceActionKind::adaptive_tuning:
      return OnlineMaintenanceOperationKind::vector_adaptive_tuning;
    case VectorMaintenanceActionKind::retrain:
      return OnlineMaintenanceOperationKind::vector_retrain;
    case VectorMaintenanceActionKind::rebuild:
      return OnlineMaintenanceOperationKind::vector_rebuild;
    case VectorMaintenanceActionKind::unknown:
      return OnlineMaintenanceOperationKind::unknown;
  }
  return OnlineMaintenanceOperationKind::unknown;
}

void AddEvidence(VectorMaintenanceJobRecord* record, std::string value) {
  if (record != nullptr) {
    record->evidence.push_back(std::move(value));
  }
}

void AddCommonEvidence(VectorMaintenanceJobRecord* record) {
  AddEvidence(record, kVectorMaintenanceJobSearchKey);
  AddEvidence(record, kVectorLatencyDeletionDriftSearchKey);
  AddEvidence(record,
              "target_collection_uuid=" + record->target_collection_uuid);
  AddEvidence(record, "target_index_uuid=" + record->target_index_uuid);
  AddEvidence(record,
              "provider_generation=" +
                  std::to_string(record->provider_generation));
  AddEvidence(record,
              "old_training_generation=" +
                  std::to_string(record->old_training_generation));
  AddEvidence(record,
              "new_training_generation=" +
                  std::to_string(record->new_training_generation));
  AddEvidence(record,
              "vector_maintenance_action=" +
                  std::string(VectorMaintenanceActionKindName(
                      record->action_kind)));
  AddEvidence(record,
              "progress_phase=" +
                  std::string(OnlineMaintenancePhaseName(
                      record->progress.snapshot.phase)));
  AddEvidence(record,
              "publish_state=" +
                  std::string(VectorMaintenancePublishStateName(
                      record->publish_state)));
  AddEvidence(record,
              "failure_class=" +
                  std::string(VectorMaintenanceFailureClassName(
                      record->failure_class)));
  AddEvidence(record,
              "retry_state=" +
                  std::string(VectorMaintenanceRetryStateName(
                      record->retry_state)));
  AddEvidence(record,
              "retry_attempts=" + std::to_string(record->retry_attempts));
  AddEvidence(record,
              "max_retry_attempts=" +
                  std::to_string(record->max_retry_attempts));
  AddEvidence(record,
              "runtime_correctness_blocker=" +
                  record->runtime_correctness_blocker);
  AddEvidence(record, "vector_maintenance_records_only=true");
  AddEvidence(record,
              std::string("exact_fallback_available=") +
                  (record->exact_fallback_available ? "true" : "false"));
  AddEvidence(record,
              std::string("exact_rerank_final_scoring_authority=") +
                  (record->exact_rerank_final_scoring_authority ? "true"
                                                                : "false"));
  AddEvidence(record,
              std::string("ann_visibility_authority=") +
                  (record->ann_visibility_authority ? "true" : "false"));
  AddEvidence(record,
              std::string("ann_finality_authority=") +
                  (record->ann_finality_authority ? "true" : "false"));
  for (const auto& lifecycle_value : record->lifecycle_evidence) {
    AddEvidence(record, "lifecycle." + lifecycle_value);
  }
}

VectorMaintenanceJobResult Finish(VectorMaintenanceJobStore* store,
                                  VectorMaintenanceJobRecord record,
                                  AgentRuntimeStatus status,
                                  bool fail_closed) {
  record.evidence.clear();
  AddCommonEvidence(&record);
  if (store != nullptr && !record.job_uuid.empty()) {
    store->Upsert(record);
  }
  VectorMaintenanceJobResult result;
  result.status = std::move(status);
  result.record = std::move(record);
  result.fail_closed = fail_closed;
  return result;
}

VectorMaintenanceJobResult Refuse(VectorMaintenanceJobStore* store,
                                  VectorMaintenanceJobRecord record,
                                  std::string code,
                                  std::string detail = {}) {
  record.publish_state = VectorMaintenancePublishState::refused;
  if (record.failure_class == VectorMaintenanceFailureClass::none) {
    record.failure_class =
        detail == "authority_boundary_refused"
            ? VectorMaintenanceFailureClass::authority_boundary_refused
            : VectorMaintenanceFailureClass::runtime_correctness_unproven;
  }
  return Finish(store,
                std::move(record),
                AgentError(std::move(code), std::move(detail)),
                true);
}

VectorMaintenanceJobRecord* FindRecord(VectorMaintenanceJobStore* store,
                                       const std::string& job_uuid) {
  return store == nullptr ? nullptr : store->MutableFind(job_uuid);
}

VectorMaintenanceJobResult RefuseMissing(std::string code) {
  return Refuse(nullptr, VectorMaintenanceJobRecord{}, std::move(code));
}

}  // namespace

AgentRuntimeStatus VectorMaintenanceJobStore::Upsert(
    VectorMaintenanceJobRecord record) {
  if (record.job_uuid.empty()) {
    return AgentError("VECTOR_MAINTENANCE_JOB.JOB_UUID_REQUIRED");
  }
  auto found = std::find_if(records_.begin(),
                            records_.end(),
                            [&](const VectorMaintenanceJobRecord& candidate) {
                              return candidate.job_uuid == record.job_uuid;
                            });
  if (found == records_.end()) {
    records_.push_back(std::move(record));
  } else {
    *found = std::move(record);
  }
  return AgentOk();
}

std::optional<VectorMaintenanceJobRecord> VectorMaintenanceJobStore::Find(
    const std::string& job_uuid) const {
  const auto found =
      std::find_if(records_.begin(),
                   records_.end(),
                   [&](const VectorMaintenanceJobRecord& record) {
                     return record.job_uuid == job_uuid;
                   });
  if (found == records_.end()) {
    return std::nullopt;
  }
  return *found;
}

VectorMaintenanceJobRecord* VectorMaintenanceJobStore::MutableFind(
    const std::string& job_uuid) {
  auto found = std::find_if(records_.begin(),
                            records_.end(),
                            [&](const VectorMaintenanceJobRecord& record) {
                              return record.job_uuid == job_uuid;
                            });
  return found == records_.end() ? nullptr : &*found;
}

const char* VectorMaintenanceActionKindName(
    VectorMaintenanceActionKind action) {
  switch (action) {
    case VectorMaintenanceActionKind::adaptive_tuning:
      return "adaptive_tuning";
    case VectorMaintenanceActionKind::retrain:
      return "retrain";
    case VectorMaintenanceActionKind::rebuild:
      return "rebuild";
    case VectorMaintenanceActionKind::unknown:
      return "unknown";
  }
  return "unknown";
}

const char* VectorMaintenancePublishStateName(
    VectorMaintenancePublishState state) {
  switch (state) {
    case VectorMaintenancePublishState::not_ready: return "not_ready";
    case VectorMaintenancePublishState::waiting_validation:
      return "waiting_validation";
    case VectorMaintenancePublishState::publish_after_validation:
      return "publish_after_validation";
    case VectorMaintenancePublishState::published: return "published";
    case VectorMaintenancePublishState::refused: return "refused";
  }
  return "refused";
}

const char* VectorMaintenanceFailureClassName(
    VectorMaintenanceFailureClass failure_class) {
  switch (failure_class) {
    case VectorMaintenanceFailureClass::none: return "none";
    case VectorMaintenanceFailureClass::transient_provider_unavailable:
      return "transient_provider_unavailable";
    case VectorMaintenanceFailureClass::permanent_validation_failed:
      return "permanent_validation_failed";
    case VectorMaintenanceFailureClass::runtime_correctness_unproven:
      return "runtime_correctness_unproven";
    case VectorMaintenanceFailureClass::authority_boundary_refused:
      return "authority_boundary_refused";
  }
  return "runtime_correctness_unproven";
}

const char* VectorMaintenanceRetryStateName(
    VectorMaintenanceRetryState state) {
  switch (state) {
    case VectorMaintenanceRetryState::none: return "none";
    case VectorMaintenanceRetryState::retry_scheduled:
      return "retry_scheduled";
    case VectorMaintenanceRetryState::retry_exhausted:
      return "retry_exhausted";
  }
  return "none";
}

VectorMaintenanceActionKind VectorMaintenanceActionFromLifecycle(
    idx::VectorTrainingRecallLifecycleAction action) {
  switch (action) {
    case idx::VectorTrainingRecallLifecycleAction::kScheduleAdaptiveTuning:
      return VectorMaintenanceActionKind::adaptive_tuning;
    case idx::VectorTrainingRecallLifecycleAction::kScheduleRetrain:
      return VectorMaintenanceActionKind::retrain;
    case idx::VectorTrainingRecallLifecycleAction::kScheduleRebuild:
      return VectorMaintenanceActionKind::rebuild;
    case idx::VectorTrainingRecallLifecycleAction::kKeepCurrentGeneration:
    case idx::VectorTrainingRecallLifecycleAction::kExactFallback:
    case idx::VectorTrainingRecallLifecycleAction::kRefuse:
      return VectorMaintenanceActionKind::unknown;
  }
  return VectorMaintenanceActionKind::unknown;
}

VectorMaintenanceJobResult CreateVectorMaintenanceJob(
    VectorMaintenanceJobStore* store,
    const VectorMaintenanceJobRequest& request) {
  if (store == nullptr) {
    return RefuseMissing("VECTOR_MAINTENANCE_JOB.STORE_REQUIRED");
  }
  VectorMaintenanceJobRecord record;
  record.job_uuid = request.job_uuid;
  record.database_uuid = request.database_uuid;
  record.target_collection_uuid = request.target_collection_uuid;
  record.target_index_uuid = request.target_index_uuid;
  record.provider_generation = request.provider_generation;
  record.old_training_generation = request.old_training_generation;
  record.new_training_generation = request.new_training_generation;
  record.action_kind = request.action_kind;
  record.publish_state = VectorMaintenancePublishState::waiting_validation;
  record.max_retry_attempts = request.max_retry_attempts;
  record.exact_fallback_available = request.exact_fallback_available;
  record.exact_rerank_final_scoring_authority =
      request.exact_rerank_final_scoring_authority;
  record.ann_visibility_authority = request.ann_visibility_authority;
  record.ann_finality_authority = request.ann_finality_authority;
  record.lifecycle_evidence = request.lifecycle_decision.evidence;

  if (record.job_uuid.empty() || record.database_uuid.empty() ||
      record.target_collection_uuid.empty() ||
      record.target_index_uuid.empty() || record.provider_generation == 0 ||
      record.old_training_generation == 0 ||
      record.new_training_generation == 0 ||
      request.total_units == 0 || !KnownAction(record.action_kind)) {
    return Refuse(store,
                  std::move(record),
                  "VECTOR_MAINTENANCE_JOB.INVALID_RECORD",
                  "job target generation action and work units required");
  }
  if (!request.lifecycle_decision.accepted ||
      VectorMaintenanceActionFromLifecycle(request.lifecycle_decision.action) !=
          record.action_kind) {
    return Refuse(store,
                  std::move(record),
                  "VECTOR_MAINTENANCE_JOB.LIFECYCLE_DECISION_MISMATCH",
                  "accepted lifecycle maintenance decision required");
  }
  if (!record.exact_fallback_available ||
      !record.exact_rerank_final_scoring_authority ||
      record.ann_visibility_authority ||
      record.ann_finality_authority) {
    record.failure_class =
        VectorMaintenanceFailureClass::authority_boundary_refused;
    return Refuse(store,
                  std::move(record),
                  "VECTOR_MAINTENANCE_JOB.AUTHORITY_REFUSED",
                  "authority_boundary_refused");
  }

  OnlineMaintenanceStartRequest start;
  start.kind = OnlineKind(record.action_kind);
  start.operation_uuid = record.job_uuid;
  start.database_uuid = record.database_uuid;
  start.target_uuid = record.target_index_uuid;
  start.stage = "vector_maintenance_job_created";
  start.work_unit_label = "vector_maintenance_units";
  start.total_units = request.total_units;
  start.now_microseconds = request.now_microseconds;
  start.engine_mga_authoritative = request.engine_mga_authoritative;
  start.durable_checkpoint_persisted =
      request.durable_checkpoint_persisted;
  start.support_bundle_sink_available =
      request.support_bundle_sink_available;
  start.observability_sink_available =
      request.observability_sink_available;
  const auto started =
      StartOnlineMaintenanceOperation(&store->progress_store(), start);
  record.progress = started.record;
  if (!started.ok()) {
    return Refuse(store,
                  std::move(record),
                  "VECTOR_MAINTENANCE_JOB.PROGRESS_START_REFUSED",
                  started.snapshot.diagnostic_code);
  }
  return Finish(store, std::move(record), AgentOk(), false);
}

VectorMaintenanceJobResult RecordVectorMaintenanceProgress(
    VectorMaintenanceJobStore* store,
    const std::string& job_uuid,
    u64 completed_units,
    u64 total_units,
    std::string stage,
    u64 now_microseconds) {
  auto* record = FindRecord(store, job_uuid);
  if (record == nullptr) {
    return RefuseMissing("VECTOR_MAINTENANCE_JOB.NOT_FOUND");
  }
  OnlineMaintenanceProgressRequest progress;
  progress.operation_uuid = job_uuid;
  progress.stage = std::move(stage);
  progress.completed_units = completed_units;
  progress.total_units = total_units;
  progress.now_microseconds = now_microseconds;
  progress.durable_checkpoint_persisted = true;
  progress.checkpoint_payload = "vector_maintenance_checkpoint";
  const auto progressed =
      RecordOnlineMaintenanceProgress(&store->progress_store(), progress);
  record->progress = progressed.record;
  if (!progressed.ok()) {
    return Refuse(store,
                  *record,
                  "VECTOR_MAINTENANCE_JOB.PROGRESS_REFUSED",
                  progressed.snapshot.diagnostic_code);
  }
  return Finish(store, *record, AgentOk(), false);
}

VectorMaintenanceJobResult CancelVectorMaintenanceJob(
    VectorMaintenanceJobStore* store,
    const std::string& job_uuid,
    std::string reason,
    u64 now_microseconds) {
  auto* record = FindRecord(store, job_uuid);
  if (record == nullptr) {
    return RefuseMissing("VECTOR_MAINTENANCE_JOB.NOT_FOUND");
  }
  OnlineMaintenanceCancelRequest cancel;
  cancel.operation_uuid = job_uuid;
  cancel.reason = std::move(reason);
  cancel.now_microseconds = now_microseconds;
  cancel.durable_checkpoint_persisted = true;
  cancel.checkpoint_payload = "vector_maintenance_cancel_checkpoint";
  cancel.release_resource_reservation = false;
  const auto cancelled =
      CancelOnlineMaintenanceOperation(&store->progress_store(), cancel);
  record->progress = cancelled.record;
  if (!cancelled.ok()) {
    return Refuse(store,
                  *record,
                  "VECTOR_MAINTENANCE_JOB.CANCEL_REFUSED",
                  cancelled.snapshot.diagnostic_code);
  }
  return Finish(store, *record, AgentOk(), false);
}

VectorMaintenanceJobResult ResumeVectorMaintenanceJob(
    VectorMaintenanceJobStore* store,
    const std::string& job_uuid,
    u64 now_microseconds) {
  auto* record = FindRecord(store, job_uuid);
  if (record == nullptr) {
    return RefuseMissing("VECTOR_MAINTENANCE_JOB.NOT_FOUND");
  }
  OnlineMaintenanceResumeRequest resume;
  resume.operation_uuid = job_uuid;
  resume.now_microseconds = now_microseconds;
  resume.engine_mga_authoritative = true;
  resume.durable_checkpoint_persisted = true;
  resume.support_bundle_sink_available = true;
  resume.observability_sink_available = true;
  const auto resumed =
      ResumeOnlineMaintenanceOperation(&store->progress_store(), resume);
  record->progress = resumed.record;
  if (!resumed.ok()) {
    return Refuse(store,
                  *record,
                  "VECTOR_MAINTENANCE_JOB.RESUME_REFUSED",
                  resumed.snapshot.diagnostic_code);
  }
  return Finish(store, *record, AgentOk(), false);
}

VectorMaintenanceJobResult MarkVectorMaintenanceValidationReady(
    VectorMaintenanceJobStore* store,
    const std::string& job_uuid,
    u64 now_microseconds) {
  auto* record = FindRecord(store, job_uuid);
  if (record == nullptr) {
    return RefuseMissing("VECTOR_MAINTENANCE_JOB.NOT_FOUND");
  }
  auto ready = RecordVectorMaintenanceProgress(store,
                                               job_uuid,
                                               record->progress.snapshot
                                                   .total_units,
                                               record->progress.snapshot
                                                   .total_units,
                                               "validated_publish_ready",
                                               now_microseconds);
  if (!ready.ok()) {
    return ready;
  }
  auto* updated = FindRecord(store, job_uuid);
  updated->validation_successful = true;
  updated->publish_state =
      VectorMaintenancePublishState::publish_after_validation;
  return Finish(store, *updated, AgentOk(), false);
}

VectorMaintenanceJobResult PublishVectorMaintenanceAfterValidation(
    VectorMaintenanceJobStore* store,
    const std::string& job_uuid,
    u64 now_microseconds) {
  auto* record = FindRecord(store, job_uuid);
  if (record == nullptr) {
    return RefuseMissing("VECTOR_MAINTENANCE_JOB.NOT_FOUND");
  }
  if (!record->validation_successful ||
      record->publish_state !=
          VectorMaintenancePublishState::publish_after_validation) {
    return Refuse(store,
                  *record,
                  "VECTOR_MAINTENANCE_JOB.PUBLISH_VALIDATION_REQUIRED",
                  "publish_after_validation_state_required");
  }
  OnlineMaintenancePublishRequest publish;
  publish.operation_uuid = job_uuid;
  publish.now_microseconds = now_microseconds;
  publish.engine_mga_authoritative = true;
  publish.durable_publication_fence_persisted = true;
  publish.authoritative_generation_validated = true;
  publish.no_partial_visibility = true;
  publish.support_bundle_sink_available = true;
  publish.observability_sink_available = true;
  publish.release_resource_reservation = false;
  const auto published =
      PublishOnlineMaintenanceOperation(&store->progress_store(), publish);
  record->progress = published.record;
  if (!published.ok()) {
    return Refuse(store,
                  *record,
                  "VECTOR_MAINTENANCE_JOB.PUBLISH_REFUSED",
                  published.snapshot.diagnostic_code);
  }
  record->publish_state = VectorMaintenancePublishState::published;
  return Finish(store, *record, AgentOk(), false);
}

VectorMaintenanceJobResult ClassifyVectorMaintenanceFailure(
    VectorMaintenanceJobStore* store,
    const std::string& job_uuid,
    VectorMaintenanceFailureClass failure_class,
    bool transient_retryable,
    u64 /*now_microseconds*/) {
  auto* record = FindRecord(store, job_uuid);
  if (record == nullptr) {
    return RefuseMissing("VECTOR_MAINTENANCE_JOB.NOT_FOUND");
  }
  record->failure_class = failure_class;
  if (transient_retryable && record->retry_attempts < record->max_retry_attempts) {
    ++record->retry_attempts;
    record->retry_state = VectorMaintenanceRetryState::retry_scheduled;
    return Finish(store, *record, AgentOk(), false);
  }
  record->retry_state = transient_retryable
                            ? VectorMaintenanceRetryState::retry_exhausted
                            : VectorMaintenanceRetryState::none;
  return Refuse(store,
                *record,
                "VECTOR_MAINTENANCE_JOB.FAILURE_CLASSIFIED",
                VectorMaintenanceFailureClassName(failure_class));
}

}  // namespace scratchbird::core::agents
