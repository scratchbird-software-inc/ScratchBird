// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SEARCH_KEY: DBLC_013AC_BACKGROUND_JOBS_SCHEDULER
// Database-local background job scheduler primitives. The scheduler coordinates
// task lifecycle only; it is not transaction finality, policy truth, storage,
// catalog, parser, SBLR, or recovery authority.

#include "agent_runtime.hpp"
#include "agent_workload_resource_quota.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace scratchbird::core::agents {

enum class BackgroundJobSchedulerState {
  not_started,
  active,
  paused,
  maintenance,
  draining,
  stopped,
  failed
};

enum class BackgroundJobState {
  registered,
  waiting,
  running,
  paused,
  retry_scheduled,
  quarantined,
  maintenance_drained,
  shutdown_drained,
  completed,
  failed,
  denied
};

enum class BackgroundJobRunOutcome {
  success,
  transient_failure,
  permanent_failure,
  cancelled
};

struct BackgroundJobAuthorityBoundary {
  bool transaction_finality_authority = false;
  bool recovery_finality_authority = false;
  bool storage_identity_authority = false;
  bool catalog_truth_authority = false;
  bool policy_truth_authority = false;
  bool parser_admission_authority = false;
  bool sblr_execution_authority = false;
  bool task_lifecycle_request_only = true;
};

struct BackgroundJobSchedulerStartup {
  std::string database_uuid;
  u64 policy_generation = 0;
  bool tx2_activation_committed = false;
  bool startup_admitted = false;
  bool scheduler_catalog_visible = false;
  bool cluster_authority_available = false;
  u64 monotonic_now_microseconds = 0;
};

struct BackgroundJobPolicy {
  std::string policy_uuid =
      DeterministicAgentRuntimeObjectUuidFromKey("background_jobs|database_local_policy");
  bool enabled = true;
  bool allow_background_jobs = true;
  bool allow_maintenance_participation = true;
  bool allow_cluster_jobs = false;
  u64 max_attempts = 3;
  u64 initial_backoff_microseconds = 1000000;
  u64 max_backoff_microseconds = 60000000;
};

struct BackgroundJobSchedule {
  std::string schedule_uuid;
  std::string schedule_name;
  std::string schedule_kind = "manual";
  std::string expression;
  std::string starts_expression;
  std::string ends_expression;
  std::string time_zone = "UTC";
  u64 interval_microseconds = 0;
  u64 next_run_after_microseconds = 0;
  bool enabled = true;
};

struct BackgroundJobDefinition {
  std::string job_uuid;
  std::string job_type;
  std::string database_uuid;
  std::string pool_id;
  WorkloadClass workload_class = WorkloadClass::background;
  WorkloadAdmissionSource source = WorkloadAdmissionSource::engine;
  WorkloadResourceVector resource_request;
  bool cluster_only = false;
  bool participates_in_maintenance = false;
  bool enabled = true;
  u64 not_before_microseconds = 0;
  std::optional<BackgroundJobSchedule> schedule;
};

struct BackgroundJobEvidence {
  std::string job_uuid;
  std::string event;
  std::string diagnostic_code;
  std::string detail;
  std::string redaction_class = "operational_redacted";
  u64 created_at_microseconds = 0;
  std::vector<std::pair<std::string, std::string>> fields;
};

struct BackgroundJobRecord {
  BackgroundJobDefinition definition;
  BackgroundJobState state = BackgroundJobState::registered;
  u64 attempts = 0;
  u64 next_run_after_microseconds = 0;
  u64 last_run_started_microseconds = 0;
  u64 last_run_finished_microseconds = 0;
  std::string active_reservation_token;
  std::string last_diagnostic_code;
};

struct BackgroundJobRunDecision {
  AgentRuntimeStatus status;
  BackgroundJobState state = BackgroundJobState::denied;
  std::string job_uuid;
  std::string reservation_token;
  BackgroundJobEvidence evidence;

  bool admitted() const { return status.ok && state == BackgroundJobState::running; }
};

const char* BackgroundJobSchedulerStateName(BackgroundJobSchedulerState state);
const char* BackgroundJobStateName(BackgroundJobState state);
const char* BackgroundJobRunOutcomeName(BackgroundJobRunOutcome outcome);

bool BackgroundJobAuthorityBoundaryValid(const BackgroundJobAuthorityBoundary& boundary);
AgentRuntimeStatus ValidateBackgroundJobAuthorityBoundary(
    const BackgroundJobAuthorityBoundary& boundary);
AgentRuntimeStatus ValidateBackgroundJobPolicy(const BackgroundJobPolicy& policy);
std::string SerializeBackgroundJobEvidence(const BackgroundJobEvidence& evidence);

class DatabaseLocalBackgroundJobScheduler {
 public:
  AgentRuntimeStatus Start(BackgroundJobSchedulerStartup startup,
                           BackgroundJobPolicy policy = {});
  AgentRuntimeStatus RegisterJob(BackgroundJobDefinition definition);
  AgentRuntimeStatus AttachScheduleToJob(const std::string& job_uuid,
                                         BackgroundJobSchedule schedule,
                                         bool create_only,
                                         u64 now_microseconds);
  AgentRuntimeStatus AlterSchedule(const std::string& schedule_uuid,
                                   BackgroundJobSchedule schedule,
                                   u64 now_microseconds);
  AgentRuntimeStatus Pause(std::string reason, u64 now_microseconds);
  AgentRuntimeStatus Resume(u64 now_microseconds);
  AgentRuntimeStatus EnterMaintenance(std::string reason,
                                      WorkloadResourceQuotaController* quota_controller,
                                      u64 now_microseconds);
  AgentRuntimeStatus ExitMaintenance(u64 now_microseconds);
  AgentRuntimeStatus BeginShutdownDrain(std::string reason,
                                        WorkloadResourceQuotaController* quota_controller,
                                        u64 now_microseconds);

  BackgroundJobRunDecision RunNextDue(WorkloadResourceQuotaController* quota_controller,
                                      u64 now_microseconds);
  AgentRuntimeStatus CompleteRunningJob(const std::string& job_uuid,
                                        BackgroundJobRunOutcome outcome,
                                        WorkloadResourceQuotaController* quota_controller,
                                        u64 now_microseconds,
                                        std::string detail = {});
  AgentRuntimeStatus CancelJobControlAction(const std::string& job_uuid,
                                            WorkloadResourceQuotaController* quota_controller,
                                            u64 now_microseconds,
                                            std::string detail = {});
  AgentRuntimeStatus RetryJobControlAction(const std::string& job_uuid,
                                           u64 now_microseconds,
                                           std::string detail = {});
  AgentRuntimeStatus SuppressJobControlAction(const std::string& job_uuid,
                                              u64 now_microseconds,
                                              std::string detail = {});

  BackgroundJobSchedulerState state() const { return state_; }
  bool cluster_authority_available() const { return startup_.cluster_authority_available; }
  const std::vector<BackgroundJobRecord>& jobs() const { return jobs_; }
  const std::vector<BackgroundJobEvidence>& evidence_log() const { return evidence_log_; }
  std::optional<BackgroundJobRecord> FindJob(const std::string& job_uuid) const;

 private:
  BackgroundJobRunDecision RefuseRun(const BackgroundJobDefinition* definition,
                                     std::string code,
                                     std::string detail,
                                     u64 now_microseconds);
  BackgroundJobEvidence MakeEvidence(const BackgroundJobDefinition* definition,
                                     std::string event,
                                     std::string code,
                                     std::string detail,
                                     u64 now_microseconds) const;
  AgentRuntimeStatus DrainActiveJobs(std::string event,
                                     std::string code,
                                     WorkloadReleaseReason release_reason,
                                     WorkloadResourceQuotaController* quota_controller,
                                     u64 now_microseconds);
  u64 BackoffForAttempt(u64 attempt) const;

  BackgroundJobSchedulerStartup startup_;
  BackgroundJobPolicy policy_;
  BackgroundJobSchedulerState state_ = BackgroundJobSchedulerState::not_started;
  std::vector<BackgroundJobRecord> jobs_;
  std::vector<BackgroundJobEvidence> evidence_log_;
  std::string drain_reason_;
};

}  // namespace scratchbird::core::agents
