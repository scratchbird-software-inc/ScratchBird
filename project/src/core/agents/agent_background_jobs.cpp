// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_background_jobs.hpp"

#include <algorithm>
#include <sstream>
#include <utility>

namespace scratchbird::core::agents {
namespace {

bool IsZero(const WorkloadResourceVector& value) {
  return value.memory_bytes == 0 && value.worker_slots == 0 && value.temp_bytes == 0 &&
         value.filespace_bytes == 0 && value.active_requests == 0 && value.open_cursors == 0 &&
         value.transaction_slots == 0 && value.buffer_bytes == 0 && value.udr_bytes == 0;
}

bool IsTerminal(BackgroundJobState state) {
  return state == BackgroundJobState::quarantined ||
         state == BackgroundJobState::completed ||
         state == BackgroundJobState::failed ||
         state == BackgroundJobState::denied;
}

void AddResourceEvidence(std::vector<std::pair<std::string, std::string>>* fields,
                         const WorkloadResourceVector& resources) {
  fields->push_back({"memory_bytes", std::to_string(resources.memory_bytes)});
  fields->push_back({"worker_slots", std::to_string(resources.worker_slots)});
  fields->push_back({"temp_bytes", std::to_string(resources.temp_bytes)});
  fields->push_back({"filespace_bytes", std::to_string(resources.filespace_bytes)});
  fields->push_back({"active_requests", std::to_string(resources.active_requests)});
  fields->push_back({"open_cursors", std::to_string(resources.open_cursors)});
  fields->push_back({"transaction_slots", std::to_string(resources.transaction_slots)});
  fields->push_back({"buffer_bytes", std::to_string(resources.buffer_bytes)});
  fields->push_back({"udr_bytes", std::to_string(resources.udr_bytes)});
}

}  // namespace

const char* BackgroundJobSchedulerStateName(BackgroundJobSchedulerState state) {
  switch (state) {
    case BackgroundJobSchedulerState::not_started: return "not_started";
    case BackgroundJobSchedulerState::active: return "active";
    case BackgroundJobSchedulerState::paused: return "paused";
    case BackgroundJobSchedulerState::maintenance: return "maintenance";
    case BackgroundJobSchedulerState::draining: return "draining";
    case BackgroundJobSchedulerState::stopped: return "stopped";
    case BackgroundJobSchedulerState::failed: return "failed";
  }
  return "failed";
}

const char* BackgroundJobStateName(BackgroundJobState state) {
  switch (state) {
    case BackgroundJobState::registered: return "registered";
    case BackgroundJobState::waiting: return "waiting";
    case BackgroundJobState::running: return "running";
    case BackgroundJobState::paused: return "paused";
    case BackgroundJobState::retry_scheduled: return "retry_scheduled";
    case BackgroundJobState::quarantined: return "quarantined";
    case BackgroundJobState::maintenance_drained: return "maintenance_drained";
    case BackgroundJobState::shutdown_drained: return "shutdown_drained";
    case BackgroundJobState::completed: return "completed";
    case BackgroundJobState::failed: return "failed";
    case BackgroundJobState::denied: return "denied";
  }
  return "denied";
}

const char* BackgroundJobRunOutcomeName(BackgroundJobRunOutcome outcome) {
  switch (outcome) {
    case BackgroundJobRunOutcome::success: return "success";
    case BackgroundJobRunOutcome::transient_failure: return "transient_failure";
    case BackgroundJobRunOutcome::permanent_failure: return "permanent_failure";
    case BackgroundJobRunOutcome::cancelled: return "cancelled";
  }
  return "unknown";
}

bool BackgroundJobAuthorityBoundaryValid(const BackgroundJobAuthorityBoundary& boundary) {
  return !boundary.transaction_finality_authority &&
         !boundary.recovery_finality_authority &&
         !boundary.storage_identity_authority &&
         !boundary.catalog_truth_authority &&
         !boundary.policy_truth_authority &&
         !boundary.parser_admission_authority &&
         !boundary.sblr_execution_authority &&
         boundary.task_lifecycle_request_only;
}

AgentRuntimeStatus ValidateBackgroundJobAuthorityBoundary(
    const BackgroundJobAuthorityBoundary& boundary) {
  if (BackgroundJobAuthorityBoundaryValid(boundary)) {
    return AgentOk();
  }
  return AgentError("BACKGROUND_JOBS.AUTHORITY_DENIED",
                    "background_job_scheduler_authority_boundary_invalid");
}

AgentRuntimeStatus ValidateBackgroundJobPolicy(const BackgroundJobPolicy& policy) {
  if (policy.policy_uuid.empty()) {
    return AgentError("BACKGROUND_JOBS.POLICY_UUID_REQUIRED");
  }
  if (!policy.enabled || !policy.allow_background_jobs) {
    return AgentError("BACKGROUND_JOBS.POLICY_DENIED", policy.policy_uuid);
  }
  if (policy.max_attempts == 0) {
    return AgentError("BACKGROUND_JOBS.POLICY_INVALID_MAX_ATTEMPTS", policy.policy_uuid);
  }
  if (policy.initial_backoff_microseconds == 0 ||
      policy.max_backoff_microseconds < policy.initial_backoff_microseconds) {
    return AgentError("BACKGROUND_JOBS.POLICY_INVALID_BACKOFF", policy.policy_uuid);
  }
  return AgentOk();
}

std::string SerializeBackgroundJobEvidence(const BackgroundJobEvidence& evidence) {
  std::ostringstream out;
  out << "job_uuid=" << evidence.job_uuid << '\n';
  out << "event=" << evidence.event << '\n';
  out << "diagnostic_code=" << evidence.diagnostic_code << '\n';
  out << "detail=" << evidence.detail << '\n';
  out << "redaction_class=" << evidence.redaction_class << '\n';
  out << "created_at_microseconds=" << evidence.created_at_microseconds << '\n';
  out << "principal=redacted\n";
  for (const auto& field : evidence.fields) {
    out << field.first << '=' << field.second << '\n';
  }
  return out.str();
}

AgentRuntimeStatus DatabaseLocalBackgroundJobScheduler::Start(
    BackgroundJobSchedulerStartup startup,
    BackgroundJobPolicy policy) {
  startup_ = std::move(startup);
  policy_ = std::move(policy);
  state_ = BackgroundJobSchedulerState::not_started;
  const auto boundary = ValidateBackgroundJobAuthorityBoundary(BackgroundJobAuthorityBoundary{});
  if (!boundary.ok) {
    state_ = BackgroundJobSchedulerState::failed;
    return boundary;
  }
  if (startup_.database_uuid.empty() ||
      startup_.policy_generation == 0 ||
      !startup_.tx2_activation_committed ||
      !startup_.startup_admitted ||
      !startup_.scheduler_catalog_visible) {
    auto evidence = MakeEvidence(nullptr, "startup_refused",
                                 "BACKGROUND_JOBS.STARTUP_AFTER_TX2_REQUIRED",
                                 "database_uuid_policy_generation_tx2_catalog_and_startup_admission_required",
                                 startup_.monotonic_now_microseconds);
    evidence_log_.push_back(std::move(evidence));
    return AgentError("BACKGROUND_JOBS.STARTUP_AFTER_TX2_REQUIRED",
                      "database-local background jobs require tx2 activation evidence");
  }
  const auto policy_status = ValidateBackgroundJobPolicy(policy_);
  if (!policy_status.ok) {
    auto evidence = MakeEvidence(nullptr, "startup_refused",
                                 policy_status.diagnostic_code,
                                 policy_status.detail,
                                 startup_.monotonic_now_microseconds);
    evidence_log_.push_back(std::move(evidence));
    return policy_status;
  }
  state_ = BackgroundJobSchedulerState::active;
  evidence_log_.push_back(MakeEvidence(nullptr, "scheduler_started",
                                       "BACKGROUND_JOBS.STARTED",
                                       "database_local_scheduler_active",
                                       startup_.monotonic_now_microseconds));
  return AgentOk();
}

AgentRuntimeStatus DatabaseLocalBackgroundJobScheduler::RegisterJob(
    BackgroundJobDefinition definition) {
  if (state_ == BackgroundJobSchedulerState::not_started ||
      state_ == BackgroundJobSchedulerState::failed ||
      state_ == BackgroundJobSchedulerState::stopped) {
    return AgentError("BACKGROUND_JOBS.SCHEDULER_NOT_ACTIVE", BackgroundJobSchedulerStateName(state_));
  }
  if (definition.job_uuid.empty() || definition.job_type.empty() ||
      definition.database_uuid.empty() || definition.pool_id.empty()) {
    return AgentError("BACKGROUND_JOBS.JOB_DEFINITION_INVALID",
                      "job_uuid_job_type_database_uuid_and_pool_id_required");
  }
  if (definition.database_uuid != startup_.database_uuid) {
    return AgentError("BACKGROUND_JOBS.DATABASE_SCOPE_MISMATCH", definition.job_uuid);
  }
  if (IsZero(definition.resource_request)) {
    return AgentError("BACKGROUND_JOBS.RESOURCE_RESERVATION_REQUIRED", definition.job_uuid);
  }
  if (definition.cluster_only && !startup_.cluster_authority_available) {
    auto evidence = MakeEvidence(&definition, "job_denied",
                                 "BACKGROUND_JOBS.CLUSTER_AUTHORITY_REQUIRED",
                                 "cluster_only_job_failed_closed_without_cluster_authority",
                                 startup_.monotonic_now_microseconds);
    evidence_log_.push_back(std::move(evidence));
    return AgentError("BACKGROUND_JOBS.CLUSTER_AUTHORITY_REQUIRED", definition.job_uuid);
  }
  const auto duplicate = std::find_if(jobs_.begin(), jobs_.end(), [&](const BackgroundJobRecord& job) {
    return job.definition.job_uuid == definition.job_uuid;
  });
  if (duplicate != jobs_.end()) {
    return AgentError("BACKGROUND_JOBS.DUPLICATE_JOB", definition.job_uuid);
  }

  BackgroundJobRecord record;
  record.next_run_after_microseconds = definition.not_before_microseconds;
  record.state = definition.enabled ? BackgroundJobState::waiting : BackgroundJobState::denied;
  record.last_diagnostic_code = definition.enabled ? "BACKGROUND_JOBS.REGISTERED"
                                                   : "BACKGROUND_JOBS.JOB_DISABLED";
  record.definition = std::move(definition);
  evidence_log_.push_back(MakeEvidence(&record.definition, "job_registered",
                                       record.last_diagnostic_code,
                                       BackgroundJobStateName(record.state),
                                       startup_.monotonic_now_microseconds));
  jobs_.push_back(std::move(record));
  return AgentOk();
}

AgentRuntimeStatus DatabaseLocalBackgroundJobScheduler::AttachScheduleToJob(
    const std::string& job_uuid,
    BackgroundJobSchedule schedule,
    bool create_only,
    u64 now_microseconds) {
  if (state_ == BackgroundJobSchedulerState::not_started ||
      state_ == BackgroundJobSchedulerState::failed ||
      state_ == BackgroundJobSchedulerState::stopped) {
    return AgentError("BACKGROUND_JOBS.SCHEDULER_NOT_ACTIVE", BackgroundJobSchedulerStateName(state_));
  }
  if (schedule.schedule_uuid.empty() || schedule.schedule_name.empty() ||
      schedule.schedule_kind.empty() || schedule.expression.empty()) {
    return AgentError("BACKGROUND_JOBS.SCHEDULE_DEFINITION_INVALID",
                      "schedule_uuid_name_kind_and_expression_required");
  }
  auto job = std::find_if(jobs_.begin(), jobs_.end(), [&](const BackgroundJobRecord& candidate) {
    return candidate.definition.job_uuid == job_uuid;
  });
  if (job == jobs_.end()) {
    return AgentError("BACKGROUND_JOBS.JOB_NOT_FOUND", job_uuid);
  }
  const auto duplicate = std::find_if(jobs_.begin(), jobs_.end(), [&](const BackgroundJobRecord& candidate) {
    return candidate.definition.schedule.has_value() &&
           candidate.definition.schedule->schedule_uuid == schedule.schedule_uuid &&
           candidate.definition.job_uuid != job_uuid;
  });
  if (duplicate != jobs_.end()) {
    return AgentError("BACKGROUND_JOBS.DUPLICATE_SCHEDULE", schedule.schedule_uuid);
  }
  if (create_only && job->definition.schedule.has_value()) {
    return AgentError("BACKGROUND_JOBS.DUPLICATE_SCHEDULE",
                      job->definition.schedule->schedule_uuid);
  }

  const bool created = !job->definition.schedule.has_value();
  job->definition.schedule = std::move(schedule);
  job->next_run_after_microseconds =
      job->definition.schedule->next_run_after_microseconds;
  job->last_diagnostic_code = created ? "BACKGROUND_JOBS.SCHEDULE_CREATED"
                                      : "BACKGROUND_JOBS.SCHEDULE_UPDATED";
  auto evidence = MakeEvidence(&job->definition,
                               created ? "job_schedule_created" : "job_schedule_updated",
                               job->last_diagnostic_code,
                               job->definition.schedule->schedule_uuid,
                               now_microseconds);
  evidence.fields.push_back({"schedule_uuid", job->definition.schedule->schedule_uuid});
  evidence.fields.push_back({"schedule_name", job->definition.schedule->schedule_name});
  evidence.fields.push_back({"schedule_kind", job->definition.schedule->schedule_kind});
  evidence.fields.push_back({"schedule_expression", job->definition.schedule->expression});
  evidence.fields.push_back({"schedule_starts_expression",
                             job->definition.schedule->starts_expression});
  evidence.fields.push_back({"schedule_ends_expression",
                             job->definition.schedule->ends_expression});
  evidence.fields.push_back({"schedule_interval_microseconds",
                             std::to_string(job->definition.schedule->interval_microseconds)});
  evidence.fields.push_back({"next_run_after_microseconds",
                             std::to_string(job->next_run_after_microseconds)});
  evidence_log_.push_back(std::move(evidence));
  return AgentOk();
}

AgentRuntimeStatus DatabaseLocalBackgroundJobScheduler::AlterSchedule(
    const std::string& schedule_uuid,
    BackgroundJobSchedule schedule,
    u64 now_microseconds) {
  if (schedule_uuid.empty()) {
    return AgentError("BACKGROUND_JOBS.SCHEDULE_ID_REQUIRED");
  }
  auto job = std::find_if(jobs_.begin(), jobs_.end(), [&](const BackgroundJobRecord& candidate) {
    return candidate.definition.schedule.has_value() &&
           candidate.definition.schedule->schedule_uuid == schedule_uuid;
  });
  if (job == jobs_.end()) {
    return AgentError("BACKGROUND_JOBS.SCHEDULE_NOT_FOUND", schedule_uuid);
  }
  if (schedule.schedule_uuid.empty()) schedule.schedule_uuid = schedule_uuid;
  if (schedule.schedule_name.empty()) {
    schedule.schedule_name = job->definition.schedule->schedule_name;
  }
  if (schedule.schedule_kind.empty()) {
    schedule.schedule_kind = job->definition.schedule->schedule_kind;
  }
  if (schedule.expression.empty()) {
    schedule.expression = job->definition.schedule->expression;
  }
  if (schedule.starts_expression.empty()) {
    schedule.starts_expression = job->definition.schedule->starts_expression;
  }
  if (schedule.ends_expression.empty()) {
    schedule.ends_expression = job->definition.schedule->ends_expression;
  }
  if (schedule.time_zone.empty()) {
    schedule.time_zone = job->definition.schedule->time_zone;
  }
  return AttachScheduleToJob(job->definition.job_uuid,
                             std::move(schedule),
                             false,
                             now_microseconds);
}

AgentRuntimeStatus DatabaseLocalBackgroundJobScheduler::Pause(std::string reason,
                                                              u64 now_microseconds) {
  if (state_ != BackgroundJobSchedulerState::active &&
      state_ != BackgroundJobSchedulerState::maintenance) {
    return AgentError("BACKGROUND_JOBS.PAUSE_NOT_ALLOWED", BackgroundJobSchedulerStateName(state_));
  }
  state_ = BackgroundJobSchedulerState::paused;
  for (auto& job : jobs_) {
    if (job.state == BackgroundJobState::waiting ||
        job.state == BackgroundJobState::retry_scheduled) {
      job.state = BackgroundJobState::paused;
      job.last_diagnostic_code = "BACKGROUND_JOBS.PAUSED";
    }
  }
  evidence_log_.push_back(MakeEvidence(nullptr, "scheduler_paused",
                                       "BACKGROUND_JOBS.PAUSED", std::move(reason),
                                       now_microseconds));
  return AgentOk();
}

AgentRuntimeStatus DatabaseLocalBackgroundJobScheduler::Resume(u64 now_microseconds) {
  if (state_ != BackgroundJobSchedulerState::paused) {
    return AgentError("BACKGROUND_JOBS.RESUME_NOT_ALLOWED", BackgroundJobSchedulerStateName(state_));
  }
  state_ = BackgroundJobSchedulerState::active;
  for (auto& job : jobs_) {
    if (job.state == BackgroundJobState::paused) {
      job.state = BackgroundJobState::waiting;
      job.last_diagnostic_code = "BACKGROUND_JOBS.RESUMED";
    }
  }
  evidence_log_.push_back(MakeEvidence(nullptr, "scheduler_resumed",
                                       "BACKGROUND_JOBS.RESUMED",
                                       "database_local_scheduler_active",
                                       now_microseconds));
  return AgentOk();
}

AgentRuntimeStatus DatabaseLocalBackgroundJobScheduler::EnterMaintenance(
    std::string reason,
    WorkloadResourceQuotaController* quota_controller,
    u64 now_microseconds) {
  if (state_ != BackgroundJobSchedulerState::active &&
      state_ != BackgroundJobSchedulerState::paused) {
    return AgentError("BACKGROUND_JOBS.MAINTENANCE_NOT_ALLOWED", BackgroundJobSchedulerStateName(state_));
  }
  if (!policy_.allow_maintenance_participation) {
    return AgentError("BACKGROUND_JOBS.MAINTENANCE_POLICY_DENIED", policy_.policy_uuid);
  }
  const auto drain = DrainActiveJobs("maintenance_drained",
                                    "BACKGROUND_JOBS.MAINTENANCE_DRAINED",
                                    WorkloadReleaseReason::cancellation,
                                    quota_controller,
                                    now_microseconds);
  if (!drain.ok) {
    return drain;
  }
  state_ = BackgroundJobSchedulerState::maintenance;
  evidence_log_.push_back(MakeEvidence(nullptr, "maintenance_entered",
                                       "BACKGROUND_JOBS.MAINTENANCE_ENTERED",
                                       std::move(reason),
                                       now_microseconds));
  return AgentOk();
}

AgentRuntimeStatus DatabaseLocalBackgroundJobScheduler::ExitMaintenance(u64 now_microseconds) {
  if (state_ != BackgroundJobSchedulerState::maintenance) {
    return AgentError("BACKGROUND_JOBS.MAINTENANCE_EXIT_NOT_ALLOWED", BackgroundJobSchedulerStateName(state_));
  }
  state_ = BackgroundJobSchedulerState::active;
  for (auto& job : jobs_) {
    if (job.state == BackgroundJobState::maintenance_drained) {
      job.state = BackgroundJobState::waiting;
      job.last_diagnostic_code = "BACKGROUND_JOBS.MAINTENANCE_EXIT_RESCHEDULED";
    }
  }
  evidence_log_.push_back(MakeEvidence(nullptr, "maintenance_exited",
                                       "BACKGROUND_JOBS.MAINTENANCE_EXITED",
                                       "database_local_scheduler_active",
                                       now_microseconds));
  return AgentOk();
}

AgentRuntimeStatus DatabaseLocalBackgroundJobScheduler::BeginShutdownDrain(
    std::string reason,
    WorkloadResourceQuotaController* quota_controller,
    u64 now_microseconds) {
  if (state_ == BackgroundJobSchedulerState::stopped) {
    return AgentOk();
  }
  state_ = BackgroundJobSchedulerState::draining;
  drain_reason_ = std::move(reason);
  const auto drain = DrainActiveJobs("shutdown_drained",
                                    "BACKGROUND_JOBS.SHUTDOWN_DRAINED",
                                    WorkloadReleaseReason::shutdown,
                                    quota_controller,
                                    now_microseconds);
  if (!drain.ok) {
    state_ = BackgroundJobSchedulerState::failed;
    return drain;
  }
  for (auto& job : jobs_) {
    if (!IsTerminal(job.state) && job.state != BackgroundJobState::shutdown_drained) {
      job.state = BackgroundJobState::shutdown_drained;
      job.last_diagnostic_code = "BACKGROUND_JOBS.SHUTDOWN_DRAINED";
    }
  }
  state_ = BackgroundJobSchedulerState::stopped;
  evidence_log_.push_back(MakeEvidence(nullptr, "scheduler_stopped",
                                       "BACKGROUND_JOBS.SHUTDOWN_DRAIN_COMPLETE",
                                       drain_reason_,
                                       now_microseconds));
  return AgentOk();
}

BackgroundJobRunDecision DatabaseLocalBackgroundJobScheduler::RunNextDue(
    WorkloadResourceQuotaController* quota_controller,
    u64 now_microseconds) {
  if (quota_controller == nullptr) {
    return RefuseRun(nullptr, "BACKGROUND_JOBS.RESOURCE_CONTROLLER_REQUIRED",
                     "quota_controller_required_before_job_start", now_microseconds);
  }
  if (state_ == BackgroundJobSchedulerState::paused) {
    return RefuseRun(nullptr, "BACKGROUND_JOBS.SCHEDULER_PAUSED",
                     "scheduler_paused", now_microseconds);
  }
  if (state_ == BackgroundJobSchedulerState::draining ||
      state_ == BackgroundJobSchedulerState::stopped) {
    return RefuseRun(nullptr, "BACKGROUND_JOBS.SHUTDOWN_DRAIN_NO_NEW_RUNS",
                     drain_reason_, now_microseconds);
  }
  if (state_ != BackgroundJobSchedulerState::active &&
      state_ != BackgroundJobSchedulerState::maintenance) {
    return RefuseRun(nullptr, "BACKGROUND_JOBS.SCHEDULER_NOT_ACTIVE",
                     BackgroundJobSchedulerStateName(state_), now_microseconds);
  }

  for (auto& job : jobs_) {
    if ((job.state != BackgroundJobState::waiting &&
         job.state != BackgroundJobState::retry_scheduled) ||
        job.next_run_after_microseconds > now_microseconds) {
      continue;
    }
    if (!job.definition.enabled) {
      job.state = BackgroundJobState::denied;
      job.last_diagnostic_code = "BACKGROUND_JOBS.JOB_DISABLED";
      return RefuseRun(&job.definition, job.last_diagnostic_code,
                       "job_disabled_by_definition", now_microseconds);
    }
    if (state_ == BackgroundJobSchedulerState::maintenance &&
        !job.definition.participates_in_maintenance) {
      job.state = BackgroundJobState::maintenance_drained;
      job.last_diagnostic_code = "BACKGROUND_JOBS.MAINTENANCE_DRAINED";
      evidence_log_.push_back(MakeEvidence(&job.definition, "maintenance_drained",
                                           job.last_diagnostic_code,
                                           "job_deferred_for_maintenance",
                                           now_microseconds));
      continue;
    }
    if (job.definition.cluster_only && !startup_.cluster_authority_available) {
      job.state = BackgroundJobState::denied;
      job.last_diagnostic_code = "BACKGROUND_JOBS.CLUSTER_AUTHORITY_REQUIRED";
      return RefuseRun(&job.definition, job.last_diagnostic_code,
                       "cluster_only_job_failed_closed_without_cluster_authority",
                       now_microseconds);
    }
    if (job.definition.cluster_only && !policy_.allow_cluster_jobs) {
      job.state = BackgroundJobState::denied;
      job.last_diagnostic_code = "BACKGROUND_JOBS.CLUSTER_POLICY_DENIED";
      return RefuseRun(&job.definition, job.last_diagnostic_code,
                       "cluster_jobs_not_enabled_by_policy",
                       now_microseconds);
    }

    WorkloadAdmissionRequest request;
    request.request_uuid = job.definition.job_uuid + ":attempt:" + std::to_string(job.attempts + 1);
    request.pool_id = job.definition.pool_id;
    request.workload_class = job.definition.workload_class;
    request.source = job.definition.source;
    request.requested = job.definition.resource_request;
    request.maintenance_override = state_ == BackgroundJobSchedulerState::maintenance &&
                                   job.definition.participates_in_maintenance;
    request.cluster_scoped = job.definition.cluster_only;
    request.cluster_authority_available = startup_.cluster_authority_available;
    request.principal_tag = "background_job_scheduler";
    const auto admitted = quota_controller->Admit(request);
    if (!admitted.status.ok || !admitted.reservation_created()) {
      job.last_diagnostic_code = admitted.diagnostic.diagnostic_code;
      return RefuseRun(&job.definition, admitted.diagnostic.diagnostic_code,
                       admitted.diagnostic.detail, now_microseconds);
    }

    ++job.attempts;
    job.state = BackgroundJobState::running;
    job.last_run_started_microseconds = now_microseconds;
    job.active_reservation_token = admitted.reservation.token_id;
    job.last_diagnostic_code = admitted.diagnostic.diagnostic_code;
    BackgroundJobRunDecision decision;
    decision.status = AgentOk();
    decision.state = job.state;
    decision.job_uuid = job.definition.job_uuid;
    decision.reservation_token = admitted.reservation.token_id;
    decision.evidence = MakeEvidence(&job.definition, "job_started",
                                     "BACKGROUND_JOBS.RUN_ADMITTED",
                                     admitted.diagnostic.diagnostic_code,
                                     now_microseconds);
    decision.evidence.fields.push_back({"reservation_token", admitted.reservation.token_id});
    AddResourceEvidence(&decision.evidence.fields, job.definition.resource_request);
    evidence_log_.push_back(decision.evidence);
    return decision;
  }

  return RefuseRun(nullptr, "BACKGROUND_JOBS.NO_DUE_JOB",
                   "no_due_database_local_background_job", now_microseconds);
}

AgentRuntimeStatus DatabaseLocalBackgroundJobScheduler::CompleteRunningJob(
    const std::string& job_uuid,
    BackgroundJobRunOutcome outcome,
    WorkloadResourceQuotaController* quota_controller,
    u64 now_microseconds,
    std::string detail) {
  auto job = std::find_if(jobs_.begin(), jobs_.end(), [&](const BackgroundJobRecord& candidate) {
    return candidate.definition.job_uuid == job_uuid;
  });
  if (job == jobs_.end()) {
    return AgentError("BACKGROUND_JOBS.JOB_NOT_FOUND", job_uuid);
  }
  if (job->state != BackgroundJobState::running || job->active_reservation_token.empty()) {
    return AgentError("BACKGROUND_JOBS.JOB_NOT_RUNNING", job_uuid);
  }
  if (quota_controller == nullptr) {
    return AgentError("BACKGROUND_JOBS.RESOURCE_CONTROLLER_REQUIRED", job_uuid);
  }

  const WorkloadReleaseReason release_reason =
      outcome == BackgroundJobRunOutcome::success ? WorkloadReleaseReason::success :
      outcome == BackgroundJobRunOutcome::cancelled ? WorkloadReleaseReason::cancellation :
      WorkloadReleaseReason::failure;
  const auto release = quota_controller->Release(job->active_reservation_token, release_reason);
  if (!release.ok) {
    return release;
  }
  job->active_reservation_token.clear();
  job->last_run_finished_microseconds = now_microseconds;

  const std::string outcome_name = BackgroundJobRunOutcomeName(outcome);
  if (outcome == BackgroundJobRunOutcome::success) {
    job->state = BackgroundJobState::completed;
    job->last_diagnostic_code = "BACKGROUND_JOBS.RUN_COMPLETED";
  } else if (outcome == BackgroundJobRunOutcome::cancelled) {
    job->state = BackgroundJobState::waiting;
    job->last_diagnostic_code = "BACKGROUND_JOBS.RUN_CANCELLED";
  } else if (outcome == BackgroundJobRunOutcome::permanent_failure ||
             job->attempts >= policy_.max_attempts) {
    job->state = BackgroundJobState::quarantined;
    job->last_diagnostic_code = "BACKGROUND_JOBS.QUARANTINED";
  } else {
    job->state = BackgroundJobState::retry_scheduled;
    job->last_diagnostic_code = "BACKGROUND_JOBS.RETRY_SCHEDULED";
    job->next_run_after_microseconds = now_microseconds + BackoffForAttempt(job->attempts);
  }

  auto evidence = MakeEvidence(&job->definition, "job_finished",
                               job->last_diagnostic_code,
                               detail.empty() ? outcome_name : std::move(detail),
                               now_microseconds);
  evidence.fields.push_back({"attempts", std::to_string(job->attempts)});
  evidence.fields.push_back({"next_run_after_microseconds",
                             std::to_string(job->next_run_after_microseconds)});
  evidence_log_.push_back(std::move(evidence));
  return AgentOk();
}

AgentRuntimeStatus DatabaseLocalBackgroundJobScheduler::CancelJobControlAction(
    const std::string& job_uuid,
    WorkloadResourceQuotaController* quota_controller,
    u64 now_microseconds,
    std::string detail) {
  auto job = std::find_if(jobs_.begin(), jobs_.end(), [&](const BackgroundJobRecord& candidate) {
    return candidate.definition.job_uuid == job_uuid;
  });
  if (job == jobs_.end()) {
    return AgentError("BACKGROUND_JOBS.JOB_NOT_FOUND", job_uuid);
  }
  if (job->state == BackgroundJobState::running) {
    return CompleteRunningJob(job_uuid,
                              BackgroundJobRunOutcome::cancelled,
                              quota_controller,
                              now_microseconds,
                              detail.empty() ? "agent_job_control_cancel" : std::move(detail));
  }
  if (IsTerminal(job->state)) {
    return AgentError("BACKGROUND_JOBS.JOB_TERMINAL", job_uuid);
  }
  job->state = BackgroundJobState::denied;
  job->last_diagnostic_code = "BACKGROUND_JOBS.CANCELLED_BY_AGENT";
  auto evidence = MakeEvidence(&job->definition,
                               "job_cancelled_by_agent",
                               job->last_diagnostic_code,
                               detail.empty() ? "agent_job_control_cancel" : std::move(detail),
                               now_microseconds);
  evidence.fields.push_back({"control_authority", "agent_job_control_manager"});
  evidence_log_.push_back(std::move(evidence));
  return AgentOk();
}

AgentRuntimeStatus DatabaseLocalBackgroundJobScheduler::RetryJobControlAction(
    const std::string& job_uuid,
    u64 now_microseconds,
    std::string detail) {
  auto job = std::find_if(jobs_.begin(), jobs_.end(), [&](const BackgroundJobRecord& candidate) {
    return candidate.definition.job_uuid == job_uuid;
  });
  if (job == jobs_.end()) {
    return AgentError("BACKGROUND_JOBS.JOB_NOT_FOUND", job_uuid);
  }
  if (job->state == BackgroundJobState::running) {
    return AgentError("BACKGROUND_JOBS.JOB_RUNNING", job_uuid);
  }
  if (job->attempts >= policy_.max_attempts) {
    return AgentError("BACKGROUND_JOBS.MAX_ATTEMPTS_EXHAUSTED", job_uuid);
  }
  if (job->definition.cluster_only && !startup_.cluster_authority_available) {
    return AgentError("BACKGROUND_JOBS.CLUSTER_AUTHORITY_REQUIRED", job_uuid);
  }
  job->state = BackgroundJobState::retry_scheduled;
  job->definition.enabled = true;
  job->next_run_after_microseconds = now_microseconds;
  job->last_diagnostic_code = "BACKGROUND_JOBS.RETRY_REQUESTED_BY_AGENT";
  auto evidence = MakeEvidence(&job->definition,
                               "job_retry_requested_by_agent",
                               job->last_diagnostic_code,
                               detail.empty() ? "agent_job_control_retry" : std::move(detail),
                               now_microseconds);
  evidence.fields.push_back({"control_authority", "agent_job_control_manager"});
  evidence.fields.push_back({"attempts", std::to_string(job->attempts)});
  evidence_log_.push_back(std::move(evidence));
  return AgentOk();
}

AgentRuntimeStatus DatabaseLocalBackgroundJobScheduler::SuppressJobControlAction(
    const std::string& job_uuid,
    u64 now_microseconds,
    std::string detail) {
  auto job = std::find_if(jobs_.begin(), jobs_.end(), [&](const BackgroundJobRecord& candidate) {
    return candidate.definition.job_uuid == job_uuid;
  });
  if (job == jobs_.end()) {
    return AgentError("BACKGROUND_JOBS.JOB_NOT_FOUND", job_uuid);
  }
  if (job->state == BackgroundJobState::running) {
    return AgentError("BACKGROUND_JOBS.JOB_RUNNING", job_uuid);
  }
  job->definition.enabled = false;
  job->state = BackgroundJobState::denied;
  job->last_diagnostic_code = "BACKGROUND_JOBS.SUPPRESSED_BY_AGENT";
  auto evidence = MakeEvidence(&job->definition,
                               "job_suppressed_by_agent",
                               job->last_diagnostic_code,
                               detail.empty() ? "agent_job_control_suppress" : std::move(detail),
                               now_microseconds);
  evidence.fields.push_back({"control_authority", "agent_job_control_manager"});
  evidence_log_.push_back(std::move(evidence));
  return AgentOk();
}

std::optional<BackgroundJobRecord> DatabaseLocalBackgroundJobScheduler::FindJob(
    const std::string& job_uuid) const {
  const auto job = std::find_if(jobs_.begin(), jobs_.end(), [&](const BackgroundJobRecord& candidate) {
    return candidate.definition.job_uuid == job_uuid;
  });
  if (job == jobs_.end()) {
    return std::nullopt;
  }
  return *job;
}

BackgroundJobRunDecision DatabaseLocalBackgroundJobScheduler::RefuseRun(
    const BackgroundJobDefinition* definition,
    std::string code,
    std::string detail,
    u64 now_microseconds) {
  BackgroundJobRunDecision decision;
  decision.status = AgentError(code, detail);
  decision.state = BackgroundJobState::denied;
  decision.job_uuid = definition == nullptr ? std::string{} : definition->job_uuid;
  decision.evidence = MakeEvidence(definition, "run_refused", std::move(code),
                                   std::move(detail), now_microseconds);
  evidence_log_.push_back(decision.evidence);
  return decision;
}

BackgroundJobEvidence DatabaseLocalBackgroundJobScheduler::MakeEvidence(
    const BackgroundJobDefinition* definition,
    std::string event,
    std::string code,
    std::string detail,
    u64 now_microseconds) const {
  BackgroundJobEvidence evidence;
  evidence.job_uuid = definition == nullptr ? std::string{"scheduler"} : definition->job_uuid;
  evidence.event = std::move(event);
  evidence.diagnostic_code = std::move(code);
  evidence.detail = std::move(detail);
  evidence.created_at_microseconds = now_microseconds;
  evidence.fields.push_back({"database_uuid", startup_.database_uuid.empty() ? "unknown" : startup_.database_uuid});
  evidence.fields.push_back({"policy_uuid", policy_.policy_uuid});
  evidence.fields.push_back({"policy_generation", std::to_string(startup_.policy_generation)});
  evidence.fields.push_back({"scheduler_state", BackgroundJobSchedulerStateName(state_)});
  if (definition != nullptr) {
    evidence.fields.push_back({"job_type", definition->job_type});
    evidence.fields.push_back({"cluster_only", definition->cluster_only ? "true" : "false"});
    evidence.fields.push_back({"participates_in_maintenance",
                               definition->participates_in_maintenance ? "true" : "false"});
  }
  return evidence;
}

AgentRuntimeStatus DatabaseLocalBackgroundJobScheduler::DrainActiveJobs(
    std::string event,
    std::string code,
    WorkloadReleaseReason release_reason,
    WorkloadResourceQuotaController* quota_controller,
    u64 now_microseconds) {
  if (quota_controller == nullptr) {
    return AgentError("BACKGROUND_JOBS.RESOURCE_CONTROLLER_REQUIRED", event);
  }
  for (auto& job : jobs_) {
    if (job.state != BackgroundJobState::running) {
      continue;
    }
    const auto release = quota_controller->Release(job.active_reservation_token, release_reason);
    if (!release.ok) {
      return release;
    }
    job.active_reservation_token.clear();
    job.last_run_finished_microseconds = now_microseconds;
    job.state = event == "shutdown_drained" ? BackgroundJobState::shutdown_drained
                                            : BackgroundJobState::maintenance_drained;
    job.last_diagnostic_code = code;
    evidence_log_.push_back(MakeEvidence(&job.definition, event, code,
                                         WorkloadReleaseReasonName(release_reason),
                                         now_microseconds));
  }
  return AgentOk();
}

u64 DatabaseLocalBackgroundJobScheduler::BackoffForAttempt(u64 attempt) const {
  u64 backoff = policy_.initial_backoff_microseconds;
  for (u64 i = 1; i < attempt && backoff < policy_.max_backoff_microseconds; ++i) {
    if (backoff > policy_.max_backoff_microseconds / 2) {
      backoff = policy_.max_backoff_microseconds;
    } else {
      backoff *= 2;
    }
  }
  return std::min(backoff, policy_.max_backoff_microseconds);
}

}  // namespace scratchbird::core::agents
