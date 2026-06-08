// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_background_jobs.hpp"

#include <cstdlib>
#include <cstdint>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Contains(std::string_view haystack, std::string_view needle) {
  return haystack.find(needle) != std::string_view::npos;
}

agents::WorkloadResourceVector Resources(std::uint64_t value) {
  agents::WorkloadResourceVector resources;
  resources.memory_bytes = value;
  resources.worker_slots = value;
  resources.temp_bytes = value;
  resources.filespace_bytes = value;
  resources.active_requests = value;
  resources.open_cursors = value;
  resources.transaction_slots = value;
  resources.buffer_bytes = value;
  resources.udr_bytes = value;
  return resources;
}

agents::WorkloadQuotaLimits Limits(std::uint64_t hard) {
  agents::WorkloadQuotaLimits limits;
  limits.hard = Resources(hard);
  limits.soft = Resources(hard);
  return limits;
}

agents::WorkloadResourcePoolConfig Pool(std::string pool_id,
                                        agents::WorkloadClass workload_class,
                                        std::uint64_t hard = 10) {
  agents::WorkloadResourcePoolConfig pool;
  pool.pool_id = std::move(pool_id);
  pool.workload_class = workload_class;
  pool.limits = Limits(hard);
  return pool;
}

agents::BackgroundJobSchedulerStartup Startup(bool tx2_committed = true,
                                              bool cluster_authority = false) {
  agents::BackgroundJobSchedulerStartup startup;
  startup.database_uuid = "019e0f2a-0000-7000-8000-0000000000ac";
  startup.policy_generation = 4;
  startup.tx2_activation_committed = tx2_committed;
  startup.startup_admitted = true;
  startup.scheduler_catalog_visible = true;
  startup.cluster_authority_available = cluster_authority;
  startup.monotonic_now_microseconds = 100;
  return startup;
}

agents::BackgroundJobPolicy Policy(std::uint64_t max_attempts = 3) {
  agents::BackgroundJobPolicy policy;
  policy.policy_uuid = "policy:background_jobs:test";
  policy.max_attempts = max_attempts;
  policy.initial_backoff_microseconds = 10;
  policy.max_backoff_microseconds = 80;
  return policy;
}

agents::BackgroundJobDefinition Job(std::string job_uuid,
                                    std::string pool_id = "background") {
  agents::BackgroundJobDefinition job;
  job.job_uuid = std::move(job_uuid);
  job.job_type = "test_job";
  job.database_uuid = Startup().database_uuid;
  job.pool_id = std::move(pool_id);
  job.workload_class = agents::WorkloadClass::background;
  job.source = agents::WorkloadAdmissionSource::engine;
  job.resource_request = Resources(1);
  return job;
}

agents::DatabaseLocalBackgroundJobScheduler StartedScheduler(
    agents::BackgroundJobPolicy policy = Policy(),
    bool cluster_authority = false) {
  agents::DatabaseLocalBackgroundJobScheduler scheduler;
  const auto started = scheduler.Start(Startup(true, cluster_authority), policy);
  Require(started.ok, "scheduler start failed");
  Require(scheduler.state() == agents::BackgroundJobSchedulerState::active,
          "scheduler did not become active");
  return scheduler;
}

agents::WorkloadResourceQuotaController Quota(std::string pool_id = "background",
                                              agents::WorkloadClass workload_class = agents::WorkloadClass::background,
                                              std::uint64_t hard = 10) {
  agents::WorkloadResourceQuotaController quota;
  Require(quota.RegisterPool(Pool(std::move(pool_id), workload_class, hard)).ok,
          "quota pool registration failed");
  return quota;
}

void TestStartupGatesAfterTx2() {
  agents::DatabaseLocalBackgroundJobScheduler scheduler;
  auto startup = Startup(false);
  const auto refused = scheduler.Start(startup, Policy());
  Require(!refused.ok, "scheduler started before tx2 activation commit");
  Require(refused.diagnostic_code == "BACKGROUND_JOBS.STARTUP_AFTER_TX2_REQUIRED",
          "startup gate diagnostic mismatch");
  Require(scheduler.state() == agents::BackgroundJobSchedulerState::not_started,
          "startup refusal did not leave scheduler not_started");

  startup.tx2_activation_committed = true;
  const auto admitted = scheduler.Start(startup, Policy());
  Require(admitted.ok, "scheduler did not start after tx2 activation evidence");
  Require(scheduler.state() == agents::BackgroundJobSchedulerState::active,
          "scheduler state not active after valid startup");
}

void TestPauseResumeLifecycle() {
  auto scheduler = StartedScheduler();
  auto quota = Quota();
  Require(scheduler.RegisterJob(Job("pause-resume")).ok, "job registration failed");
  Require(scheduler.Pause("operator pause", 120).ok, "scheduler pause failed");
  const auto paused = scheduler.RunNextDue(&quota, 130);
  Require(!paused.status.ok &&
              paused.status.diagnostic_code == "BACKGROUND_JOBS.SCHEDULER_PAUSED",
          "paused scheduler admitted work");
  Require(quota.ActiveReservationCount() == 0, "pause path created a reservation");
  Require(scheduler.Resume(140).ok, "scheduler resume failed");
  const auto running = scheduler.RunNextDue(&quota, 150);
  Require(running.admitted(), "resumed scheduler did not admit due job");
  Require(quota.ActiveReservationCount() == 1, "resumed job did not reserve resources");
  Require(scheduler.CompleteRunningJob("pause-resume", agents::BackgroundJobRunOutcome::success,
                                       &quota, 160)
              .ok,
          "resumed job completion failed");
}

void TestRetryBackoffAndQuarantine() {
  auto scheduler = StartedScheduler(Policy(2));
  auto quota = Quota();
  Require(scheduler.RegisterJob(Job("retry-quarantine")).ok, "retry job registration failed");

  const auto first = scheduler.RunNextDue(&quota, 200);
  Require(first.admitted(), "first retry test run not admitted");
  Require(scheduler.CompleteRunningJob("retry-quarantine",
                                       agents::BackgroundJobRunOutcome::transient_failure,
                                       &quota, 205, "transient fault")
              .ok,
          "first transient failure completion failed");
  auto job = scheduler.FindJob("retry-quarantine");
  Require(job.has_value() &&
              job->state == agents::BackgroundJobState::retry_scheduled &&
              job->next_run_after_microseconds == 215,
          "transient failure did not schedule expected backoff");

  const auto too_early = scheduler.RunNextDue(&quota, 214);
  Require(!too_early.status.ok &&
              too_early.status.diagnostic_code == "BACKGROUND_JOBS.NO_DUE_JOB",
          "scheduler ignored retry backoff");

  const auto second = scheduler.RunNextDue(&quota, 215);
  Require(second.admitted(), "second retry test run not admitted");
  Require(scheduler.CompleteRunningJob("retry-quarantine",
                                       agents::BackgroundJobRunOutcome::transient_failure,
                                       &quota, 216, "second transient fault")
              .ok,
          "second transient failure completion failed");
  job = scheduler.FindJob("retry-quarantine");
  Require(job.has_value() && job->state == agents::BackgroundJobState::quarantined,
          "max retry failure did not quarantine job");
  const auto evidence = agents::SerializeBackgroundJobEvidence(scheduler.evidence_log().back());
  Require(Contains(evidence, "diagnostic_code=BACKGROUND_JOBS.QUARANTINED"),
          "quarantine evidence missing diagnostic");
  Require(Contains(evidence, "redaction_class=operational_redacted"),
          "quarantine evidence missing redaction class");
}

void TestMaintenanceDrainAndParticipation() {
  auto scheduler = StartedScheduler();
  auto quota = Quota();
  Require(scheduler.RegisterJob(Job("normal-maintenance-drain")).ok,
          "normal maintenance-drain job registration failed");
  const auto running = scheduler.RunNextDue(&quota, 300);
  Require(running.admitted(), "normal maintenance-drain job not admitted");
  Require(quota.ActiveReservationCount() == 1, "normal job did not reserve resources");
  Require(scheduler.EnterMaintenance("repair window", &quota, 310).ok,
          "maintenance entry failed");
  Require(scheduler.state() == agents::BackgroundJobSchedulerState::maintenance,
          "scheduler did not enter maintenance state");
  Require(quota.ActiveReservationCount() == 0, "maintenance drain leaked reservation");
  auto drained = scheduler.FindJob("normal-maintenance-drain");
  Require(drained.has_value() &&
              drained->state == agents::BackgroundJobState::maintenance_drained,
          "normal job was not maintenance-drained");

  auto maintenance = Job("maintenance-participant");
  maintenance.participates_in_maintenance = true;
  Require(scheduler.RegisterJob(maintenance).ok, "maintenance participant registration failed");
  const auto maintenance_run = scheduler.RunNextDue(&quota, 320);
  Require(maintenance_run.admitted(), "maintenance participant not admitted");
  Require(scheduler.CompleteRunningJob("maintenance-participant",
                                       agents::BackgroundJobRunOutcome::success,
                                       &quota, 330)
              .ok,
          "maintenance participant completion failed");
  Require(scheduler.ExitMaintenance(340).ok, "maintenance exit failed");
  Require(scheduler.state() == agents::BackgroundJobSchedulerState::active,
          "scheduler did not return to active after maintenance");
}

void TestShutdownDrain() {
  auto scheduler = StartedScheduler();
  auto quota = Quota();
  Require(scheduler.RegisterJob(Job("shutdown-drain")).ok, "shutdown job registration failed");
  const auto running = scheduler.RunNextDue(&quota, 400);
  Require(running.admitted(), "shutdown job was not admitted");
  Require(scheduler.BeginShutdownDrain("operator shutdown", &quota, 410).ok,
          "shutdown drain failed");
  Require(scheduler.state() == agents::BackgroundJobSchedulerState::stopped,
          "shutdown drain did not stop scheduler");
  Require(quota.ActiveReservationCount() == 0, "shutdown drain leaked reservation");
  const auto refused = scheduler.RunNextDue(&quota, 420);
  Require(!refused.status.ok &&
              refused.status.diagnostic_code ==
                  "BACKGROUND_JOBS.SHUTDOWN_DRAIN_NO_NEW_RUNS",
          "stopped scheduler admitted new work");
}

void TestPolicyDenial() {
  agents::DatabaseLocalBackgroundJobScheduler scheduler;
  auto policy = Policy();
  policy.enabled = false;
  const auto denied = scheduler.Start(Startup(), policy);
  Require(!denied.ok &&
              denied.diagnostic_code == "BACKGROUND_JOBS.POLICY_DENIED",
          "disabled policy did not deny scheduler startup");

  agents::BackgroundJobAuthorityBoundary invalid;
  invalid.transaction_finality_authority = true;
  const auto authority = agents::ValidateBackgroundJobAuthorityBoundary(invalid);
  Require(!authority.ok &&
              authority.diagnostic_code == "BACKGROUND_JOBS.AUTHORITY_DENIED",
          "invalid scheduler authority boundary was accepted");
}

void TestResourceDenial() {
  auto scheduler = StartedScheduler();
  auto quota = Quota("background", agents::WorkloadClass::background, 1);
  auto oversized = Job("resource-denied");
  oversized.resource_request = Resources(2);
  Require(scheduler.RegisterJob(oversized).ok, "oversized job registration failed");
  const auto denied = scheduler.RunNextDue(&quota, 500);
  Require(!denied.status.ok &&
              denied.status.diagnostic_code == "WORKLOAD_RESOURCE.HARD_DENIED",
          "resource hard denial did not propagate to scheduler");
  Require(!denied.admitted(), "resource-denied job was marked admitted");
  Require(quota.ActiveReservationCount() == 0, "resource denial created reservation");
}

void TestClusterFailClosed() {
  auto scheduler = StartedScheduler();
  auto cluster_job = Job("cluster-fail-closed");
  cluster_job.cluster_only = true;
  cluster_job.workload_class = agents::WorkloadClass::cluster;
  cluster_job.pool_id = "cluster";
  const auto denied = scheduler.RegisterJob(cluster_job);
  Require(!denied.ok &&
              denied.diagnostic_code == "BACKGROUND_JOBS.CLUSTER_AUTHORITY_REQUIRED",
          "cluster-only job did not fail closed without cluster authority");

  auto cluster_scheduler = StartedScheduler(Policy(), true);
  auto policy = Policy();
  policy.allow_cluster_jobs = true;
  cluster_scheduler = StartedScheduler(policy, true);
  auto quota = Quota("cluster", agents::WorkloadClass::cluster, 10);
  auto admitted_cluster_job = cluster_job;
  admitted_cluster_job.job_uuid = "cluster-with-authority";
  Require(cluster_scheduler.RegisterJob(admitted_cluster_job).ok,
          "cluster job with authority registration failed");
  const auto running = cluster_scheduler.RunNextDue(&quota, 600);
  Require(running.admitted(), "cluster job with authority was not admitted");
  Require(cluster_scheduler.CompleteRunningJob("cluster-with-authority",
                                              agents::BackgroundJobRunOutcome::success,
                                              &quota, 610)
              .ok,
          "cluster authority job completion failed");
}

}  // namespace

int main() {
  TestStartupGatesAfterTx2();
  TestPauseResumeLifecycle();
  TestRetryBackoffAndQuarantine();
  TestMaintenanceDrainAndParticipation();
  TestShutdownDrain();
  TestPolicyDenial();
  TestResourceDenial();
  TestClusterFailClosed();
  return EXIT_SUCCESS;
}
