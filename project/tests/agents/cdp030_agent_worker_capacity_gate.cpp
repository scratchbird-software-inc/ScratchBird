// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "agent_background_jobs.hpp"
#include "agent_runtime.hpp"
#include "agent_workload_resource_quota.hpp"

#include <cstdlib>
#include <iostream>
#include <string>

namespace {

namespace agents = scratchbird::core::agents;

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

agents::AgentRuntimeContext Context() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.cluster_authority_available = false;
  context.database_uuid = "019f0300-0000-7000-8000-000000000001";
  context.principal_uuid = "019f0300-0000-7000-8000-000000000002";
  context.rights = {
      "OBS_AGENT_STATE_READ",
      "OBS_AGENT_CONTROL",
      "OBS_AGENT_EVIDENCE_READ"};
  context.monotonic_now_microseconds = 1000000;
  return context;
}

const agents::AgentWorkerCapacityAssignment* FindAssignment(
    const agents::AgentWorkerCapacitySnapshot& snapshot,
    const std::string& agent_type_id) {
  for (const auto& assignment : snapshot.assignments) {
    if (assignment.agent_type_id == agent_type_id) {
      return &assignment;
    }
  }
  return nullptr;
}

agents::WorkloadResourcePoolConfig Pool(
    std::string pool_id,
    agents::WorkloadClass workload_class,
    scratchbird::core::platform::u64 worker_slots) {
  agents::WorkloadResourcePoolConfig pool;
  pool.pool_id = std::move(pool_id);
  pool.workload_class = workload_class;
  pool.limits.hard.worker_slots = worker_slots;
  pool.limits.hard.memory_bytes = 1024 * 1024;
  pool.limits.hard.active_requests = worker_slots;
  return pool;
}

agents::BackgroundJobSchedulerStartup Startup() {
  agents::BackgroundJobSchedulerStartup startup;
  startup.database_uuid = "019f0300-0000-7000-8000-000000000001";
  startup.policy_generation = 30;
  startup.tx2_activation_committed = true;
  startup.startup_admitted = true;
  startup.scheduler_catalog_visible = true;
  startup.cluster_authority_available = false;
  startup.monotonic_now_microseconds = 1000000;
  return startup;
}

agents::BackgroundJobDefinition AgentJob(
    std::string job_uuid,
    std::string job_type,
    scratchbird::core::platform::u64 not_before_microseconds) {
  agents::BackgroundJobDefinition job;
  job.job_uuid = std::move(job_uuid);
  job.job_type = std::move(job_type);
  job.database_uuid = "019f0300-0000-7000-8000-000000000001";
  job.pool_id = "agent-background";
  job.workload_class = agents::WorkloadClass::background;
  job.source = agents::WorkloadAdmissionSource::engine;
  job.resource_request.worker_slots = 1;
  job.resource_request.memory_bytes = 4096;
  job.resource_request.active_requests = 1;
  job.not_before_microseconds = not_before_microseconds;
  return job;
}

void TestWorkerCapacitySnapshot() {
  agents::AgentWorkerCapacityConfig config;
  config.observed_cpu_count = 5;
  config.configured_cpu_count = 5;
  config.foreground_reserved_capacity = 1;
  config.max_background_worker_slots = 4;
  config.foreground_database_work_active = false;
  config.standalone_edition = true;
  config.cluster_authority_available = false;

  const auto snapshot = agents::PlanAgentWorkerCapacity(
      config, Context(), agents::DefaultDmlPreworkAgentWorkerCandidates(30));
  Require(snapshot.status.ok, "worker capacity planner failed");
  Require(snapshot.observed_cpu_count == 5, "observed CPU evidence mismatch");
  Require(snapshot.configured_cpu_count == 5,
          "configured CPU evidence mismatch");
  Require(snapshot.effective_cpu_count == 5, "effective CPU mismatch");
  Require(snapshot.foreground_reserved_capacity == 1,
          "foreground reserved capacity mismatch");
  Require(snapshot.foreground_capacity_reserved,
          "foreground reservation was not recorded");
  Require(snapshot.background_worker_slots == 4,
          "background worker slot count mismatch");

  const auto* page = FindAssignment(snapshot, "page_allocation_manager");
  const auto* filespace = FindAssignment(snapshot, "filespace_capacity_manager");
  const auto* storage = FindAssignment(snapshot, "storage_health_manager");
  const auto* tx = FindAssignment(snapshot, "transaction_pressure_manager");
  Require(page != nullptr && filespace != nullptr && storage != nullptr &&
              tx != nullptr,
          "missing DML pre-work agent assignment");
  Require(page->assigned && filespace->assigned && storage->assigned &&
              tx->assigned,
          "DML pre-work agent was not assigned");
  Require(page->worker_slot_index != filespace->worker_slot_index,
          "page/filespace agents shared a worker slot");
  Require(page->can_run_before_foreground_demand &&
              filespace->can_run_before_foreground_demand,
          "page/filespace agents cannot run before foreground demand");
  Require(tx->cluster_path_failed_closed,
          "transaction pressure local worker did not mark cluster path failed closed");
  Require(page->diagnostic_code == "ENGINE.AGENT_WORKER_CAPACITY.ASSIGNED",
          "page assignment diagnostic mismatch");
  Require(!page->evidence_uuid.empty(), "page assignment evidence missing");
}

void TestWorkerCapacityRefusalDiagnostics() {
  agents::AgentWorkerCapacityConfig config;
  config.observed_cpu_count = 2;
  config.configured_cpu_count = 2;
  config.foreground_reserved_capacity = 1;
  config.max_background_worker_slots = 1;
  config.foreground_database_work_active = true;
  config.standalone_edition = true;
  config.cluster_authority_available = false;

  auto candidates = agents::DefaultDmlPreworkAgentWorkerCandidates(30);
  candidates.resize(1);
  auto foreground = agents::PlanAgentWorkerCapacity(config, Context(), candidates);
  const auto* blocked = FindAssignment(foreground, "page_allocation_manager");
  Require(blocked != nullptr, "missing foreground-blocked assignment");
  Require(!blocked->assigned, "foreground protection assigned background work");
  Require(blocked->diagnostic_code ==
              "SB_AGENT_RESOURCE_BUDGET.FOREGROUND_PROTECTION",
          "foreground protection diagnostic mismatch");
  Require(foreground.resource_bounds_blocked_work,
          "foreground protection was not marked as resource-bound");

  config.foreground_database_work_active = false;
  candidates[0].usage.cpu_time_microseconds = 101;
  candidates[0].policy.config_fields["max_cpu_time_microseconds"] = "100";
  auto cpu = agents::PlanAgentWorkerCapacity(config, Context(), candidates);
  const auto* throttled = FindAssignment(cpu, "page_allocation_manager");
  Require(throttled != nullptr, "missing CPU-throttled assignment");
  Require(!throttled->assigned, "CPU-throttled work was assigned");
  Require(throttled->diagnostic_code ==
              "SB_AGENT_RESOURCE_BUDGET.CPU_TIME_EXCEEDED",
          "CPU throttle diagnostic mismatch");
  Require(cpu.resource_bounds_blocked_work,
          "CPU throttle was not marked as resource-bound");
}

void TestBackgroundAgentAdmissionBeforeForegroundDemand() {
  agents::WorkloadResourceQuotaController quota;
  Require(quota.RegisterPool(Pool("agent-background",
                                  agents::WorkloadClass::background, 2))
              .ok,
          "background pool registration failed");
  Require(quota.RegisterPool(Pool("foreground-dml",
                                  agents::WorkloadClass::foreground, 1))
              .ok,
          "foreground pool registration failed");

  agents::DatabaseLocalBackgroundJobScheduler scheduler;
  Require(scheduler.Start(Startup()).ok, "background scheduler start failed");
  Require(scheduler.RegisterJob(AgentJob("cdp030-page", "page_allocation_manager", 1))
              .ok,
          "page job registration failed");
  Require(scheduler.RegisterJob(AgentJob("cdp030-filespace",
                                         "filespace_capacity_manager", 1))
              .ok,
          "filespace job registration failed");

  const auto page = scheduler.RunNextDue(&quota, 2);
  Require(page.admitted(), "page job was not admitted before foreground DML");
  const auto filespace = scheduler.RunNextDue(&quota, 3);
  Require(filespace.admitted(),
          "filespace job was not admitted before foreground DML");

  agents::WorkloadAdmissionRequest foreground;
  foreground.request_uuid = "cdp030-foreground-dml";
  foreground.pool_id = "foreground-dml";
  foreground.workload_class = agents::WorkloadClass::foreground;
  foreground.source = agents::WorkloadAdmissionSource::engine;
  foreground.requested.worker_slots = 1;
  foreground.requested.memory_bytes = 4096;
  foreground.requested.active_requests = 1;
  foreground.principal_tag = "foreground_dml";
  const auto admitted = quota.Admit(foreground);
  Require(admitted.status.ok && admitted.reservation_created(),
          "background agents consumed protected foreground capacity");
  Require(quota.UsageForPool("agent-background").worker_slots == 2,
          "background worker usage mismatch");
  Require(quota.UsageForPool("foreground-dml").worker_slots == 1,
          "foreground protected slot was not reserved independently");
}

}  // namespace

int main() {
  TestWorkerCapacitySnapshot();
  TestWorkerCapacityRefusalDiagnostics();
  TestBackgroundAgentAdmissionBeforeForegroundDemand();
  return EXIT_SUCCESS;
}
