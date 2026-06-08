// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

// SEARCH_KEY: DPC_AGENT_SCHEDULER_FAIRNESS_GATE

#include "agent_background_jobs.hpp"
#include "agent_runtime.hpp"
#include "agent_workload_resource_quota.hpp"
#include "database_lifecycle.hpp"
#include "server_agent_runtime.hpp"
#include "uuid.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace db = scratchbird::storage::database;
namespace platform = scratchbird::core::platform;
namespace server = scratchbird::server;
namespace uuid = scratchbird::core::uuid;

struct EvidenceRow {
  std::string surface;
  std::string key;
  std::string value;
};

[[noreturn]] void Fail(const std::string& message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, const std::string& message) {
  if (!condition) { Fail(message); }
}

std::string DiagnosticSummary(
    const std::vector<server::ServerDiagnostic>& diagnostics) {
  std::string out;
  for (const auto& diagnostic : diagnostics) {
    if (!out.empty()) { out += ";"; }
    out += diagnostic.code;
    if (!diagnostic.safe_message.empty()) {
      out += ":" + diagnostic.safe_message;
    }
    for (const auto& field : diagnostic.fields) {
      out += ":" + field.key + "=" + field.value;
    }
  }
  return out.empty() ? "none" : out;
}

std::string TypedUuid(platform::UuidKind kind, platform::u64 seed) {
  const auto generated = uuid::GenerateEngineIdentityV7(kind, seed);
  Require(generated.ok(), "typed UUID generation failed");
  return uuid::UuidToString(generated.value.value);
}

agents::AgentRuntimeContext Context() {
  agents::AgentRuntimeContext context;
  context.security_context_present = true;
  context.private_features_available = true;
  context.standalone_edition = true;
  context.cluster_authority_available = false;
  context.database_uuid = TypedUuid(platform::UuidKind::database, 36001);
  context.principal_uuid =
      agents::DeterministicAgentRuntimePrincipalUuidFromKey("dpc-036");
  context.groups = {"OPS"};
  context.rights = {
      "OBS_AGENT_STATE_READ",
      "OBS_AGENT_CONTROL",
      "OBS_AGENT_EVIDENCE_READ",
  };
  context.monotonic_now_microseconds = 3600000;
  context.wall_now_microseconds = 1700000000000360ull;
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
    platform::u64 worker_slots,
    platform::u64 active_requests) {
  agents::WorkloadResourcePoolConfig pool;
  pool.pool_id = std::move(pool_id);
  pool.workload_class = workload_class;
  pool.limits.hard.worker_slots = worker_slots;
  pool.limits.hard.memory_bytes = 1024 * 1024;
  pool.limits.hard.filespace_bytes = 1024 * 1024;
  pool.limits.hard.open_cursors = 64;
  pool.limits.hard.active_requests = active_requests;
  return pool;
}

agents::WorkloadAdmissionRequest Request(
    std::string request_uuid,
    std::string pool_id,
    agents::WorkloadClass workload_class) {
  agents::WorkloadAdmissionRequest request;
  request.request_uuid = std::move(request_uuid);
  request.pool_id = std::move(pool_id);
  request.workload_class = workload_class;
  request.source = agents::WorkloadAdmissionSource::engine;
  request.requested.worker_slots = 1;
  request.requested.memory_bytes = 4096;
  request.requested.active_requests = 1;
  request.principal_tag = "dpc036";
  return request;
}

agents::BackgroundJobSchedulerStartup Startup(const std::string& database_uuid) {
  agents::BackgroundJobSchedulerStartup startup;
  startup.database_uuid = database_uuid;
  startup.policy_generation = 36;
  startup.tx2_activation_committed = true;
  startup.startup_admitted = true;
  startup.scheduler_catalog_visible = true;
  startup.cluster_authority_available = false;
  startup.monotonic_now_microseconds = 3600000;
  return startup;
}

agents::BackgroundJobDefinition Job(
    std::string job_uuid,
    std::string job_type,
    const std::string& database_uuid) {
  agents::BackgroundJobDefinition job;
  job.job_uuid = std::move(job_uuid);
  job.job_type = std::move(job_type);
  job.database_uuid = database_uuid;
  job.pool_id = "dpc036-background";
  job.workload_class = agents::WorkloadClass::background;
  job.source = agents::WorkloadAdmissionSource::engine;
  job.resource_request.worker_slots = 1;
  job.resource_request.memory_bytes = 4096;
  job.resource_request.active_requests = 1;
  job.not_before_microseconds = 1;
  return job;
}

bool HasEvidenceRow(const std::vector<EvidenceRow>& rows,
                    const std::string& surface,
                    const std::string& key) {
  for (const auto& row : rows) {
    if (row.surface == surface && row.key == key && !row.value.empty()) {
      return true;
    }
  }
  return false;
}

bool HasQuotaDiagnostic(const agents::WorkloadResourceQuotaController& quota,
                        const std::string& code) {
  for (const auto& evidence : quota.evidence_log()) {
    if (evidence.diagnostic_code == code) {
      return true;
    }
  }
  return false;
}

bool HasSchedulerEvidence(const agents::DatabaseLocalBackgroundJobScheduler& scheduler,
                          const std::string& code) {
  for (const auto& evidence : scheduler.evidence_log()) {
    if (evidence.diagnostic_code == code) {
      return true;
    }
  }
  return false;
}

agents::WorkloadResourcePoolConfig GovernorPool(
    std::string pool_id,
    std::optional<platform::u64> memory_limit = std::nullopt,
    std::optional<platform::u64> worker_limit = std::nullopt,
    std::optional<platform::u64> filespace_limit = std::nullopt,
    std::optional<platform::u64> open_cursor_limit = std::nullopt) {
  agents::WorkloadResourcePoolConfig pool;
  pool.pool_id = std::move(pool_id);
  pool.workload_class = agents::WorkloadClass::background;
  pool.limits.hard.memory_bytes = memory_limit.value_or(1024 * 1024);
  pool.limits.hard.worker_slots = worker_limit.value_or(64);
  pool.limits.hard.filespace_bytes = filespace_limit.value_or(1024 * 1024);
  pool.limits.hard.open_cursors = open_cursor_limit.value_or(64);
  pool.limits.hard.active_requests = 64;
  return pool;
}

void TestWorkerCapacityAndForegroundReservation(std::vector<EvidenceRow>* rows) {
  agents::AgentWorkerCapacityConfig config;
  config.observed_cpu_count = 4;
  config.configured_cpu_count = 4;
  config.foreground_reserved_capacity = 1;
  config.max_background_worker_slots = 3;
  config.foreground_database_work_active = false;
  config.standalone_edition = true;
  config.cluster_authority_available = false;

  const auto snapshot = agents::PlanAgentWorkerCapacity(
      config, Context(), agents::DefaultDmlPreworkAgentWorkerCandidates(36));
  Require(snapshot.status.ok, "DPC-036 capacity snapshot failed");
  Require(snapshot.foreground_capacity_reserved,
          "DPC-036 foreground capacity was not reserved");
  Require(snapshot.foreground_reserved_capacity == 1,
          "DPC-036 foreground reservation count mismatch");
  Require(snapshot.background_worker_slots == 3,
          "DPC-036 background worker slot count mismatch");

  const auto* page = FindAssignment(snapshot, "page_allocation_manager");
  const auto* filespace = FindAssignment(snapshot, "filespace_capacity_manager");
  const auto* storage = FindAssignment(snapshot, "storage_health_manager");
  Require(page != nullptr && filespace != nullptr && storage != nullptr,
          "DPC-036 DML prework assignments missing");
  Require(page->assigned && filespace->assigned && storage->assigned,
          "DPC-036 prework agents were not assigned");
  Require(page->worker_slot_index != filespace->worker_slot_index,
          "DPC-036 page/filespace agents did not receive separate workers");
  Require(filespace->worker_slot_index != storage->worker_slot_index,
          "DPC-036 filespace/storage agents did not receive separate workers");
  Require(page->can_run_before_foreground_demand &&
              filespace->can_run_before_foreground_demand,
          "DPC-036 agents cannot run ahead of foreground demand");

  rows->push_back({"status", "separate_worker_thread_evidence",
                   "page=" + std::to_string(page->worker_slot_index) +
                       ",filespace=" +
                       std::to_string(filespace->worker_slot_index)});
  rows->push_back({"status", "foreground_reserved_capacity",
                   std::to_string(snapshot.foreground_reserved_capacity)});

  config.foreground_database_work_active = true;
  const auto foreground_active = agents::PlanAgentWorkerCapacity(
      config, Context(), agents::DefaultDmlPreworkAgentWorkerCandidates(36));
  const auto* blocked = FindAssignment(foreground_active, "page_allocation_manager");
  Require(blocked != nullptr, "DPC-036 foreground-active assignment missing");
  Require(!blocked->assigned,
          "DPC-036 foreground-active protection assigned background work");
  Require(blocked->diagnostic_code ==
              "SB_AGENT_RESOURCE_BUDGET.FOREGROUND_PROTECTION",
          "DPC-036 foreground protection diagnostic mismatch");
  rows->push_back({"support_bundle", "starvation_prevention",
                   blocked->diagnostic_code});
}

void TestBackgroundProgressAndForegroundNonStarvation(std::vector<EvidenceRow>* rows) {
  const std::string database_uuid = TypedUuid(platform::UuidKind::database, 36002);
  agents::WorkloadResourceQuotaController quota;
  Require(quota.RegisterPool(Pool("dpc036-background",
                                  agents::WorkloadClass::background, 2, 2))
              .ok,
          "DPC-036 background pool registration failed");
  Require(quota.RegisterPool(Pool("dpc036-foreground",
                                  agents::WorkloadClass::foreground, 1, 1))
              .ok,
          "DPC-036 foreground pool registration failed");

  agents::DatabaseLocalBackgroundJobScheduler scheduler;
  Require(scheduler.Start(Startup(database_uuid)).ok,
          "DPC-036 scheduler start failed");
  Require(scheduler.RegisterJob(Job("dpc036-page", "page_allocation_manager",
                                    database_uuid))
              .ok,
          "DPC-036 page job registration failed");
  Require(scheduler.RegisterJob(Job("dpc036-filespace",
                                    "filespace_capacity_manager",
                                    database_uuid))
              .ok,
          "DPC-036 filespace job registration failed");

  const auto foreground = quota.Admit(Request("dpc036-foreground-active",
                                             "dpc036-foreground",
                                             agents::WorkloadClass::foreground));
  Require(foreground.status.ok && foreground.reservation_created(),
          "DPC-036 foreground reservation failed before background work");

  const auto first = scheduler.RunNextDue(&quota, 2);
  Require(first.admitted(), "DPC-036 first background job did not progress");
  Require(scheduler.CompleteRunningJob(first.job_uuid,
                                       agents::BackgroundJobRunOutcome::success,
                                       &quota,
                                       3,
                                       "foreground_dml_active")
              .ok,
          "DPC-036 first background job completion failed");

  const auto second = scheduler.RunNextDue(&quota, 4);
  Require(second.admitted(),
          "DPC-036 second background job did not progress while foreground active");
  Require(quota.UsageForPool("dpc036-foreground").worker_slots == 1,
          "DPC-036 foreground reservation was disturbed by background work");

  Require(quota.Release(foreground.reservation.token_id,
                        agents::WorkloadReleaseReason::success)
              .ok,
          "DPC-036 foreground release failed");
  const auto foreground_followup =
      quota.Admit(Request("dpc036-foreground-followup",
                          "dpc036-foreground",
                          agents::WorkloadClass::foreground));
  Require(foreground_followup.status.ok &&
              foreground_followup.reservation_created(),
          "DPC-036 foreground followup was starved by background work");
  Require(quota.UsageForPool("dpc036-background").worker_slots == 1,
          "DPC-036 background worker reservation disappeared unexpectedly");
  Require(quota.Release(foreground_followup.reservation.token_id,
                        agents::WorkloadReleaseReason::success)
              .ok,
          "DPC-036 foreground followup release failed");
  Require(scheduler.CompleteRunningJob(second.job_uuid,
                                       agents::BackgroundJobRunOutcome::success,
                                       &quota,
                                       5,
                                       "foreground_dml_active")
              .ok,
          "DPC-036 second background job completion failed");

  const auto page = scheduler.FindJob("dpc036-page");
  const auto filespace = scheduler.FindJob("dpc036-filespace");
  Require(page.has_value() && filespace.has_value(),
          "DPC-036 completed jobs missing");
  Require(page->state == agents::BackgroundJobState::completed &&
              filespace->state == agents::BackgroundJobState::completed,
          "DPC-036 background jobs did not complete");
  Require(HasSchedulerEvidence(scheduler, "BACKGROUND_JOBS.RUN_ADMITTED"),
          "DPC-036 scheduler admission evidence missing");
  Require(HasQuotaDiagnostic(quota, "WORKLOAD_RESOURCE.ADMITTED"),
          "DPC-036 quota admission evidence missing");

  rows->push_back({"support_bundle", "queue_progress",
                   "completed=2,foreground_active=true"});
  rows->push_back({"support_bundle", "last_action",
                   "BACKGROUND_JOBS.RUN_COMPLETED"});
  rows->push_back({"support_bundle", "starvation_prevention",
                   "foreground_followup_admitted_while_background_active"});
}

void TestResourceGovernorDimensions(std::vector<EvidenceRow>* rows) {
  agents::WorkloadResourceQuotaController quota;
  Require(quota.RegisterPool(GovernorPool("governor-memory", 1024)).ok,
          "DPC-036 memory governor pool failed");
  Require(quota.RegisterPool(GovernorPool("governor-worker",
                                          std::nullopt, 1))
              .ok,
          "DPC-036 worker governor pool failed");
  Require(quota.RegisterPool(GovernorPool("governor-disk",
                                          std::nullopt,
                                          std::nullopt,
                                          4096))
              .ok,
          "DPC-036 disk governor pool failed");
  Require(quota.RegisterPool(GovernorPool("governor-fd",
                                          std::nullopt,
                                          std::nullopt,
                                          std::nullopt,
                                          2))
              .ok,
          "DPC-036 FD governor pool failed");

  auto memory = Request("dpc036-memory-hard-denied",
                        "governor-memory",
                        agents::WorkloadClass::background);
  memory.requested.memory_bytes = 2048;
  Require(quota.Admit(memory).diagnostic.resource_name == "memory_bytes",
          "DPC-036 memory hard-denial evidence mismatch");

  auto worker = Request("dpc036-worker-hard-denied",
                        "governor-worker",
                        agents::WorkloadClass::background);
  worker.requested.worker_slots = 2;
  Require(quota.Admit(worker).diagnostic.resource_name == "worker_slots",
          "DPC-036 worker hard-denial evidence mismatch");

  auto disk = Request("dpc036-disk-hard-denied",
                      "governor-disk",
                      agents::WorkloadClass::background);
  disk.requested.filespace_bytes = 8192;
  Require(quota.Admit(disk).diagnostic.resource_name == "filespace_bytes",
          "DPC-036 disk hard-denial evidence mismatch");

  auto fd = Request("dpc036-fd-hard-denied",
                    "governor-fd",
                    agents::WorkloadClass::background);
  fd.requested.open_cursors = 3;
  Require(quota.Admit(fd).diagnostic.resource_name == "open_cursors",
          "DPC-036 FD hard-denial evidence mismatch");

  agents::WorkloadResourcePoolConfig queue_pool =
      Pool("governor-queue", agents::WorkloadClass::background, 2, 2);
  queue_pool.limits.soft.active_requests = 1;
  queue_pool.limits.queue_on_soft_limit = true;
  queue_pool.limits.max_queued_requests = 1;
  Require(quota.RegisterPool(queue_pool).ok,
          "DPC-036 queue governor pool failed");
  const auto active = quota.Admit(Request("dpc036-queue-active",
                                         "governor-queue",
                                         agents::WorkloadClass::background));
  Require(active.status.ok && active.reservation_created(),
          "DPC-036 queue active reservation failed");
  const auto queued = quota.Admit(Request("dpc036-queue-soft-queued",
                                         "governor-queue",
                                         agents::WorkloadClass::background));
  Require(queued.decision == agents::WorkloadAdmissionDecisionClass::queued,
          "DPC-036 queue soft-limit did not queue");
  const auto full = quota.Admit(Request("dpc036-queue-full",
                                       "governor-queue",
                                       agents::WorkloadClass::background));
  Require(full.diagnostic.diagnostic_code == "WORKLOAD_RESOURCE.QUEUE_FULL",
          "DPC-036 queue-full diagnostic mismatch");

  const auto descriptor = agents::FindAgentType("page_allocation_manager");
  Require(descriptor.has_value(),
          "DPC-036 page_allocation_manager descriptor missing");
  auto policy = agents::BaselinePolicyForAgent(*descriptor);
  agents::AgentResourceBudgetEvaluationInput budget;
  budget.budget = agents::DefaultAgentResourceBudgetForPolicy(policy);
  budget.budget.max_cpu_time_microseconds = 10;
  budget.usage.cpu_time_microseconds = 11;
  budget.usage.thread_slots = 1;
  const auto cpu =
      agents::EvaluateAgentResourceBudget(*descriptor, policy, Context(), budget);
  Require(cpu.decision == agents::AgentResourceBudgetDecisionKind::throttle_defer,
          "DPC-036 CPU budget did not throttle");
  Require(cpu.status.diagnostic_code ==
              "SB_AGENT_RESOURCE_BUDGET.CPU_TIME_EXCEEDED",
          "DPC-036 CPU budget diagnostic mismatch");

  rows->push_back({"support_bundle", "throttling",
                   "cpu,memory,worker,disk,fd,queue"});
  rows->push_back({"evidence", "queue_progress",
                   "queued_requests=" +
                       std::to_string(quota.QueuedRequestCount("governor-queue"))});
}

void TestDisabledAndSafeModePreserveForeground(std::vector<EvidenceRow>* rows) {
  agents::DatabaseLocalBackgroundJobScheduler disabled_scheduler;
  auto disabled_policy = agents::BackgroundJobPolicy{};
  disabled_policy.enabled = false;
  disabled_policy.allow_background_jobs = false;
  const auto disabled =
      disabled_scheduler.Start(Startup(TypedUuid(platform::UuidKind::database, 36003)),
                               disabled_policy);
  Require(!disabled.ok &&
              disabled.diagnostic_code == "BACKGROUND_JOBS.POLICY_DENIED",
          "DPC-036 disabled scheduler did not fail closed");

  agents::AgentInstanceRecord instance;
  instance.instance_uuid =
      agents::DeterministicAgentRuntimeObjectUuidFromKey("dpc036-safe-mode");
  instance.agent_type_id = "page_allocation_manager";
  instance.state = agents::AgentLifecycleState::safe_mode;
  instance.safe_mode = true;
  agents::AgentActionRequest action;
  action.action_uuid = "dpc036-safe-mode-action";
  action.agent_type_id = "page_allocation_manager";
  action.instance_uuid = instance.instance_uuid;
  action.operation_id = "preallocate_page_family";
  action.idempotency_key = "dpc036-safe-mode";
  action.dry_run = false;
  const auto blocked = agents::ValidateAgentSafeMode(instance, action);
  Require(!blocked.ok &&
              blocked.diagnostic_code == "SB_AGENT_SAFE_MODE.ACTION_BLOCKED",
          "DPC-036 safe mode did not block live action");
  action.dry_run = true;
  Require(agents::ValidateAgentSafeMode(instance, action).ok,
          "DPC-036 safe mode blocked dry-run evidence");

  agents::WorkloadResourceQuotaController quota;
  Require(quota.RegisterPool(Pool("safe-foreground",
                                  agents::WorkloadClass::foreground, 1, 1))
              .ok,
          "DPC-036 safe foreground pool registration failed");
  const auto foreground =
      quota.Admit(Request("dpc036-safe-foreground",
                          "safe-foreground",
                          agents::WorkloadClass::foreground));
  Require(foreground.status.ok && foreground.reservation_created(),
          "DPC-036 disabled/safe mode starved foreground work");
  rows->push_back({"support_bundle", "safe_mode_disabled_behavior",
                   "foreground_admitted_live_agent_action_blocked"});
}

void TestShutdownDrainEvidence(std::vector<EvidenceRow>* rows) {
  const std::string database_uuid = TypedUuid(platform::UuidKind::database, 36004);
  agents::WorkloadResourceQuotaController quota;
  Require(quota.RegisterPool(Pool("dpc036-background",
                                  agents::WorkloadClass::background, 1, 1))
              .ok,
          "DPC-036 shutdown pool registration failed");

  agents::DatabaseLocalBackgroundJobScheduler scheduler;
  Require(scheduler.Start(Startup(database_uuid)).ok,
          "DPC-036 shutdown scheduler start failed");
  Require(scheduler.RegisterJob(Job("dpc036-shutdown-running",
                                    "storage_version_cleanup_agent",
                                    database_uuid))
              .ok,
          "DPC-036 shutdown job registration failed");
  const auto running = scheduler.RunNextDue(&quota, 2);
  Require(running.admitted(), "DPC-036 shutdown job did not start");
  Require(quota.ActiveReservationCount() == 1,
          "DPC-036 active reservation missing before shutdown");

  Require(scheduler.BeginShutdownDrain("dpc036_shutdown",
                                       &quota,
                                       3)
              .ok,
          "DPC-036 scheduler shutdown drain failed");
  Require(scheduler.state() == agents::BackgroundJobSchedulerState::stopped,
          "DPC-036 scheduler did not stop after drain");
  Require(quota.ActiveReservationCount() == 0,
          "DPC-036 shutdown drain left active reservations");
  const auto drained = scheduler.FindJob("dpc036-shutdown-running");
  Require(drained.has_value() &&
              drained->state == agents::BackgroundJobState::shutdown_drained,
          "DPC-036 shutdown-drained job state missing");
  Require(HasSchedulerEvidence(scheduler, "BACKGROUND_JOBS.SHUTDOWN_DRAINED"),
          "DPC-036 scheduler shutdown evidence missing");
  Require(HasQuotaDiagnostic(quota, "WORKLOAD_RESOURCE.RELEASED.shutdown"),
          "DPC-036 quota shutdown release evidence missing");
  const auto refused = scheduler.RunNextDue(&quota, 4);
  Require(!refused.status.ok &&
              refused.status.diagnostic_code ==
                  "BACKGROUND_JOBS.SHUTDOWN_DRAIN_NO_NEW_RUNS",
          "DPC-036 shutdown drain admitted new work");

  quota.BeginShutdownDrain("dpc036_quota_shutdown");
  const auto drain_releases = quota.DrainForShutdown();
  Require(drain_releases.empty(),
          "DPC-036 quota drain found corrupt leftover reservations");
  const auto new_admission =
      quota.Admit(Request("dpc036-after-shutdown",
                          "dpc036-background",
                          agents::WorkloadClass::background));
  Require(new_admission.diagnostic.diagnostic_code ==
              "WORKLOAD_RESOURCE.SHUTDOWN_DRAIN_NO_NEW_ADMISSION",
          "DPC-036 quota shutdown admitted new work");

  rows->push_back({"evidence", "shutdown_drain",
                   "shutdown_drained,no_leftover_reservations"});
}

void TestLiveServerRuntimeStatusEvidence(std::vector<EvidenceRow>* rows) {
  const auto root = std::filesystem::temp_directory_path() /
                    "scratchbird-dpc036-agent-runtime";
  std::error_code ignored;
  std::filesystem::remove_all(root, ignored);
  std::filesystem::create_directories(root / "control", ignored);
  Require(!ignored, "DPC-036 temp control directory creation failed");

  server::ServerBootstrapConfig config;
  config.control_dir = root / "control";
  config.embedded_direct_mode = false;

  const auto database_uuid_value =
      uuid::GenerateEngineIdentityV7(platform::UuidKind::database, 36005);
  const auto filespace_uuid_value =
      uuid::GenerateEngineIdentityV7(platform::UuidKind::filespace, 36006);
  Require(database_uuid_value.ok(), "DPC-036 database UUID generation failed");
  Require(filespace_uuid_value.ok(), "DPC-036 filespace UUID generation failed");
  db::DatabaseCreateConfig create;
  create.path = (root / "dpc036.sbdb").string();
  create.database_uuid = database_uuid_value.value;
  create.filespace_uuid = filespace_uuid_value.value;
  create.page_size = 16384;
  create.creation_unix_epoch_millis = 1790000003600;
  create.allow_minimal_resource_bootstrap = true;
  create.require_resource_seed_pack = false;
  create.allow_overwrite = true;
  Require(db::CreateDatabaseFile(create).ok(),
          "DPC-036 database creation failed");

  server::HostedDatabaseSnapshot database;
  database.state = server::HostedDatabaseState::kOpen;
  database.database_open = true;
  database.database_path = create.path;
  database.database_uuid = uuid::UuidToString(database_uuid_value.value.value);
  database.filespace_uuid = uuid::UuidToString(filespace_uuid_value.value.value);
  database.write_admission_fenced = false;
  database.config_policy_security_lifecycle_present = true;
  database.policy_generation = 1;
  database.security_epoch = 1;
  database.cache_invalidation_epoch = 1;
  database.selected_agent_type_ids = {
      "page_allocation_manager",
      "filespace_capacity_manager",
  };

  server::HostedEngineState engine_state;
  engine_state.engine_context_active = true;
  engine_state.databases.push_back(database);

  std::vector<server::ServerDiagnostic> diagnostics;
  server::ServerAgentRuntime runtime;
  const bool runtime_started = runtime.Start(config, engine_state, &diagnostics);
  Require(runtime_started,
          "DPC-036 server agent runtime failed to start: " +
              DiagnosticSummary(diagnostics));

  server::ServerAgentRuntimeSnapshot snapshot;
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(4);
  do {
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    snapshot = runtime.Snapshot();
  } while (std::chrono::steady_clock::now() < deadline &&
           (snapshot.scheduler_ticks < 2 ||
            snapshot.worker_thread_count < 2 ||
            snapshot.durable_lease_count < snapshot.worker_thread_count ||
            snapshot.total_worker_ticks <
                snapshot.scheduler_ticks * snapshot.worker_thread_count));

  Require(snapshot.started, "DPC-036 server runtime snapshot not started");
  Require(snapshot.foreground_reserved_capacity >= 1,
          "DPC-036 server runtime foreground capacity missing");
  Require(snapshot.worker_thread_count >= 2,
          "DPC-036 server runtime worker thread count too low");
  Require(snapshot.background_worker_slots == snapshot.worker_thread_count,
          "DPC-036 server runtime background slot mismatch");
  Require(snapshot.total_worker_ticks >=
              snapshot.scheduler_ticks * snapshot.worker_thread_count,
          "DPC-036 server runtime workers were not woken fairly");
  Require(snapshot.status_path.filename() == "sb_server.agent_runtime.json",
          "DPC-036 server runtime status path mismatch");
  Require(snapshot.durable_catalog_generation > 0,
          "DPC-036 durable runtime catalog generation missing");
  Require(snapshot.durable_lease_count >= snapshot.worker_thread_count,
          "DPC-036 durable worker leases missing: leases=" +
              std::to_string(snapshot.durable_lease_count) +
              ",workers=" + std::to_string(snapshot.worker_thread_count));
  Require(!snapshot.durable_catalog_root_digest.empty(),
          "DPC-036 durable catalog root digest missing");

  runtime.Stop();
  const auto stopped = runtime.Snapshot();
  Require(!stopped.started,
          "DPC-036 server runtime did not stop cleanly");
  Require(stopped.durable_service_evidence_count >=
              snapshot.durable_service_evidence_count,
          "DPC-036 durable service evidence regressed on shutdown");

  std::string status_json;
  {
    std::ifstream in(snapshot.status_path);
    Require(static_cast<bool>(in), "DPC-036 server runtime status file missing");
    status_json.assign(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
  }
  Require(status_json.find("\"scheduler_thread_name\":\"sb-agent-sch\"") !=
              std::string::npos,
          "DPC-036 scheduler thread status evidence missing");
  Require(status_json.find("\"role\":\"database_local_agent_worker\"") !=
              std::string::npos,
          "DPC-036 worker role status evidence missing");
  Require(status_json.find("\"last_action\":") != std::string::npos,
          "DPC-036 last action status evidence missing");
  Require(status_json.find("\"durable_catalog_root_digest\":\"") !=
              std::string::npos,
          "DPC-036 durable catalog root digest status evidence missing");
  Require(status_json.find("\"lease_uuid\":\"") != std::string::npos,
          "DPC-036 durable worker lease status evidence missing");

  rows->push_back({"status", "durable_worker_runtime_evidence",
                   "workers=" + std::to_string(snapshot.worker_thread_count)});
  rows->push_back({"status", "last_action",
                   "server_agent_runtime_status_json"});
  std::filesystem::remove_all(root, ignored);
}

void RequireEvidenceRows(const std::vector<EvidenceRow>& rows) {
  Require(HasEvidenceRow(rows, "support_bundle", "throttling"),
          "DPC-036 throttling support row missing");
  Require(HasEvidenceRow(rows, "support_bundle", "starvation_prevention"),
          "DPC-036 starvation-prevention support row missing");
  Require(HasEvidenceRow(rows, "status", "durable_worker_runtime_evidence"),
          "DPC-036 durable worker runtime status row missing");
  Require(HasEvidenceRow(rows, "support_bundle", "last_action") ||
              HasEvidenceRow(rows, "status", "last_action"),
          "DPC-036 last-action evidence row missing");
  Require(HasEvidenceRow(rows, "support_bundle", "queue_progress") ||
              HasEvidenceRow(rows, "evidence", "queue_progress"),
          "DPC-036 queue-progress evidence row missing");
  Require(HasEvidenceRow(rows, "evidence", "shutdown_drain"),
          "DPC-036 shutdown-drain evidence row missing");
}

}  // namespace

int main() {
  std::vector<EvidenceRow> rows;
  TestWorkerCapacityAndForegroundReservation(&rows);
  TestBackgroundProgressAndForegroundNonStarvation(&rows);
  TestResourceGovernorDimensions(&rows);
  TestDisabledAndSafeModePreserveForeground(&rows);
  TestShutdownDrainEvidence(&rows);
  TestLiveServerRuntimeStatusEvidence(&rows);
  RequireEvidenceRows(rows);
  return EXIT_SUCCESS;
}
