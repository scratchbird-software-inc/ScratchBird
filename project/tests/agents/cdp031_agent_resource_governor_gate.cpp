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
using scratchbird::core::platform::u64;

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
  context.database_uuid = "019f0310-0000-7000-8000-000000000001";
  context.principal_uuid = "019f0310-0000-7000-8000-000000000002";
  context.rights = {
      "OBS_AGENT_STATE_READ",
      "OBS_AGENT_CONTROL",
      "OBS_AGENT_EVIDENCE_READ"};
  context.monotonic_now_microseconds = 1000000;
  return context;
}

agents::AgentPolicy PagePolicy() {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager descriptor missing");
  auto policy = agents::BaselinePolicyForAgent(*descriptor);
  policy.policy_generation = 31;
  return policy;
}

agents::AgentResourceBudgetEvaluationInput BudgetInput(
    const agents::AgentPolicy& policy) {
  agents::AgentResourceBudgetEvaluationInput input;
  input.budget = agents::DefaultAgentResourceBudgetForPolicy(policy);
  input.budget.max_cpu_time_microseconds = 100;
  input.budget.max_memory_bytes = 1000;
  input.budget.max_io_bytes = 1000;
  input.budget.max_io_ops = 10;
  input.budget.max_thread_slots = 1;
  input.budget.max_queue_depth = 2;
  input.budget.min_run_interval_microseconds = 100;
  input.budget.retry_backoff_microseconds = 200;
  input.budget.watchdog_timeout_microseconds = 500;
  input.budget.max_history_query_rows = 4;
  input.budget.max_evidence_fanout = 4;
  input.budget.max_label_cardinality = 4;
  input.usage.cpu_time_microseconds = 1;
  input.usage.memory_bytes = 1;
  input.usage.io_bytes = 1;
  input.usage.io_ops = 1;
  input.usage.thread_slots = 1;
  input.usage.queue_depth = 1;
  input.usage.runtime_microseconds = 1;
  input.usage.history_query_rows = 1;
  input.usage.evidence_fanout = 1;
  input.usage.label_cardinality = 1;
  return input;
}

void RequireBudget(agents::AgentResourceBudgetEvaluationInput input,
                   const std::string& expected_code,
                   agents::AgentResourceBudgetDecisionKind expected_kind) {
  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(), "page_allocation_manager descriptor missing");
  const auto policy = PagePolicy();
  const auto decision =
      agents::EvaluateAgentResourceBudget(*descriptor, policy, Context(), input);
  Require(decision.decision == expected_kind,
          "budget decision kind mismatch for " + expected_code);
  Require(decision.status.diagnostic_code == expected_code,
          "budget diagnostic mismatch: " + decision.status.diagnostic_code);
  Require(!decision.evidence_uuid.empty(), "budget evidence UUID missing");
  Require(decision.action_allowed ==
              (expected_kind == agents::AgentResourceBudgetDecisionKind::allow),
          "budget action_allowed flag mismatch");
}

agents::WorkloadResourcePoolConfig HardPool(std::string pool_id,
                                            std::string resource_name,
                                            u64 limit) {
  agents::WorkloadResourcePoolConfig pool;
  pool.pool_id = std::move(pool_id);
  pool.workload_class = agents::WorkloadClass::background;
  if (resource_name == "temp_bytes") {
    pool.limits.hard.temp_bytes = limit;
  } else if (resource_name == "filespace_bytes") {
    pool.limits.hard.filespace_bytes = limit;
  } else if (resource_name == "open_cursors") {
    pool.limits.hard.open_cursors = limit;
  } else if (resource_name == "worker_slots") {
    pool.limits.hard.worker_slots = limit;
  }
  return pool;
}

agents::WorkloadAdmissionRequest Request(std::string request_uuid,
                                         std::string pool_id) {
  agents::WorkloadAdmissionRequest request;
  request.request_uuid = std::move(request_uuid);
  request.pool_id = std::move(pool_id);
  request.workload_class = agents::WorkloadClass::background;
  request.source = agents::WorkloadAdmissionSource::engine;
  request.principal_tag = "cdp031-agent-governor";
  return request;
}

void RequireHardDenied(const std::string& resource_name,
                       const agents::WorkloadResourceVector& resources) {
  agents::WorkloadResourceQuotaController quota;
  Require(quota.RegisterPool(HardPool("pool-" + resource_name,
                                      resource_name, 1))
              .ok,
          "hard pool registration failed");
  auto request = Request("request-" + resource_name, "pool-" + resource_name);
  request.requested = resources;
  const auto result = quota.Admit(request);
  Require(!result.status.ok, "hard limit unexpectedly admitted " + resource_name);
  Require(result.diagnostic.diagnostic_code == "WORKLOAD_RESOURCE.HARD_DENIED",
          "hard denied diagnostic mismatch for " + resource_name);
  Require(result.diagnostic.resource_name == resource_name,
          "hard denied resource mismatch for " + resource_name);
}

agents::BackgroundJobSchedulerStartup Startup() {
  agents::BackgroundJobSchedulerStartup startup;
  startup.database_uuid = "019f0310-0000-7000-8000-000000000001";
  startup.policy_generation = 31;
  startup.tx2_activation_committed = true;
  startup.startup_admitted = true;
  startup.scheduler_catalog_visible = true;
  startup.cluster_authority_available = false;
  startup.monotonic_now_microseconds = 1000000;
  return startup;
}

void TestExactAgentBudgetDiagnostics() {
  const auto policy = PagePolicy();
  const auto base = BudgetInput(policy);

  auto foreground = base;
  foreground.foreground_database_work_active = true;
  RequireBudget(foreground, "SB_AGENT_RESOURCE_BUDGET.FOREGROUND_PROTECTION",
                agents::AgentResourceBudgetDecisionKind::foreground_protection);

  auto cpu = base;
  cpu.usage.cpu_time_microseconds = 101;
  RequireBudget(cpu, "SB_AGENT_RESOURCE_BUDGET.CPU_TIME_EXCEEDED",
                agents::AgentResourceBudgetDecisionKind::throttle_defer);

  auto memory = base;
  memory.usage.memory_bytes = 1001;
  RequireBudget(memory, "SB_AGENT_RESOURCE_BUDGET.MEMORY_BYTES_EXCEEDED",
                agents::AgentResourceBudgetDecisionKind::shed_refuse);

  auto io = base;
  io.usage.io_bytes = 1001;
  RequireBudget(io, "SB_AGENT_RESOURCE_BUDGET.IO_BYTES_EXCEEDED",
                agents::AgentResourceBudgetDecisionKind::throttle_defer);

  auto threads = base;
  threads.usage.thread_slots = 2;
  RequireBudget(threads, "SB_AGENT_RESOURCE_BUDGET.THREAD_SLOTS_EXHAUSTED",
                agents::AgentResourceBudgetDecisionKind::shed_refuse);

  auto queue = base;
  queue.usage.queue_depth = 3;
  RequireBudget(queue, "SB_AGENT_RESOURCE_BUDGET.QUEUE_DEPTH_EXCEEDED",
                agents::AgentResourceBudgetDecisionKind::shed_refuse);

  auto history = base;
  history.usage.history_query_rows = 5;
  RequireBudget(history, "SB_AGENT_RESOURCE_BUDGET.HISTORY_ROWS_EXCEEDED",
                agents::AgentResourceBudgetDecisionKind::fail_closed);

  auto evidence = base;
  evidence.usage.evidence_fanout = 5;
  RequireBudget(evidence, "SB_AGENT_RESOURCE_BUDGET.EVIDENCE_FANOUT_EXCEEDED",
                agents::AgentResourceBudgetDecisionKind::fail_closed);
}

void TestQuotaBackpressureForDiskFdAndQueueGrowth() {
  agents::WorkloadResourceVector temp;
  temp.temp_bytes = 2;
  RequireHardDenied("temp_bytes", temp);

  agents::WorkloadResourceVector filespace;
  filespace.filespace_bytes = 2;
  RequireHardDenied("filespace_bytes", filespace);

  agents::WorkloadResourceVector fd_proxy;
  fd_proxy.open_cursors = 2;
  RequireHardDenied("open_cursors", fd_proxy);

  agents::WorkloadResourceQuotaController quota;
  agents::WorkloadResourcePoolConfig pool;
  pool.pool_id = "queue-pool";
  pool.workload_class = agents::WorkloadClass::background;
  pool.limits.hard.worker_slots = 10;
  pool.limits.soft.worker_slots = 1;
  pool.limits.queue_on_soft_limit = true;
  pool.limits.max_queued_requests = 1;
  Require(quota.RegisterPool(pool).ok, "queue pool registration failed");

  auto first = Request("queue-first", "queue-pool");
  first.requested.worker_slots = 1;
  Require(quota.Admit(first).reservation_created(),
          "first queue fixture reservation failed");

  auto second = Request("queue-second", "queue-pool");
  second.requested.worker_slots = 1;
  const auto queued = quota.Admit(second);
  Require(queued.status.ok &&
              queued.decision == agents::WorkloadAdmissionDecisionClass::queued,
          "soft limit did not queue second request");

  auto third = Request("queue-third", "queue-pool");
  third.requested.worker_slots = 1;
  const auto full = quota.Admit(third);
  Require(!full.status.ok, "queue-full request unexpectedly admitted");
  Require(full.diagnostic.diagnostic_code == "WORKLOAD_RESOURCE.QUEUE_FULL",
          "queue-full diagnostic mismatch");
  Require(full.diagnostic.resource_name == "worker_slots",
          "queue-full resource mismatch");
}

void TestClusterOnlyAgentsFailClosedInStandaloneBuild() {
  agents::DatabaseLocalBackgroundJobScheduler scheduler;
  Require(scheduler.Start(Startup()).ok, "scheduler start failed");

  agents::BackgroundJobDefinition job;
  job.job_uuid = "cdp031-cluster-only-job";
  job.job_type = "cluster_scheduler_manager";
  job.database_uuid = "019f0310-0000-7000-8000-000000000001";
  job.pool_id = "cluster-background";
  job.workload_class = agents::WorkloadClass::cluster;
  job.source = agents::WorkloadAdmissionSource::cluster_remote;
  job.resource_request.worker_slots = 1;
  job.cluster_only = true;
  const auto status = scheduler.RegisterJob(job);
  Require(!status.ok, "cluster-only background job was accepted");
  Require(status.diagnostic_code == "BACKGROUND_JOBS.CLUSTER_AUTHORITY_REQUIRED",
          "cluster-only job diagnostic mismatch: " + status.diagnostic_code);

  agents::WorkloadResourceQuotaController quota;
  Require(quota.RegisterPool(HardPool("cluster-pool", "worker_slots", 1)).ok,
          "cluster quota pool registration failed");
  auto request = Request("cluster-quota", "cluster-pool");
  request.requested.worker_slots = 1;
  request.cluster_scoped = true;
  request.cluster_authority_available = false;
  const auto refused = quota.Admit(request);
  Require(!refused.status.ok, "cluster-scoped quota request accepted");
  Require(refused.diagnostic.diagnostic_code ==
              "WORKLOAD_RESOURCE.CLUSTER_AUTHORITY_UNAVAILABLE",
          "cluster-scoped quota diagnostic mismatch");
}

}  // namespace

int main() {
  TestExactAgentBudgetDiagnostics();
  TestQuotaBackpressureForDiskFdAndQueueGrowth();
  TestClusterOnlyAgentsFailClosedInStandaloneBuild();
  return EXIT_SUCCESS;
}
