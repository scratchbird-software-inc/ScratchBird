// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "compression_policy.hpp"
#include "agent_background_jobs.hpp"
#include "nosql/nosql_physical_provider_contract.hpp"
#include "optimized_path_resource_governance.hpp"
#include "streaming_cursor_manager.hpp"
#include "vector_maintenance_jobs.hpp"
#include "vector_training_recall_lifecycle.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace idx = scratchbird::core::index;
namespace nosql = scratchbird::engine::internal_api;
namespace wire = scratchbird::wire;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ORH-124 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool Has(const std::vector<std::string>& values, std::string_view expected) {
  return std::find(values.begin(), values.end(), expected) != values.end();
}

bool HasPrefix(const std::vector<std::string>& values,
               std::string_view prefix) {
  return std::any_of(values.begin(), values.end(), [&](const auto& value) {
    return value.rfind(prefix, 0) == 0;
  });
}

agents::ResourceGovernanceQuotaVector QuotaVector(std::int64_t scale) {
  agents::ResourceGovernanceQuotaVector quota;
  quota.memory_bytes = 1024 * scale;
  quota.device_memory_bytes = scale;
  quota.pinned_memory_bytes = scale;
  quota.io_bytes = 2048 * scale;
  quota.io_ops = 16 * scale;
  quota.worker_threads = scale;
  quota.backlog_items = 8 * scale;
  quota.candidate_rows = 256 * scale;
  quota.cache_entries = 8 * scale;
  quota.batch_rows = 32 * scale;
  quota.fragments = scale;
  quota.lanes = scale;
  quota.time_budget_microseconds = 1000 * scale;
  return quota;
}

agents::ResourceGovernanceAdmissionRequest GovernanceRequest(
    std::string operation_id,
    agents::ResourceGovernanceFamily family,
    agents::ResourceGovernanceAction over_limit_action =
        agents::ResourceGovernanceAction::kFailClosed) {
  agents::ResourceGovernanceAdmissionRequest request;
  request.operation_id = std::move(operation_id);
  request.expected_family = family;
  request.descriptor.descriptor_id = request.operation_id + ".quota";
  request.descriptor.family = family;
  request.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.descriptor.source_path_or_label = "runtime_policy/orh124";
  request.descriptor.descriptor_generation = 124;
  request.descriptor.expected_generation = 124;
  request.descriptor.limits = QuotaVector(4);
  request.descriptor.over_limit_action = over_limit_action;
  request.descriptor.benchmark_clean = true;
  request.descriptor.runtime_dependency_present = true;
  request.requested = QuotaVector(1);
  request.require_exact_scalar_fallback_available =
      over_limit_action == agents::ResourceGovernanceAction::kExactScalarFallback;
  request.exact_scalar_fallback_available = true;
  return request;
}

agents::WorkloadResourceVector WorkloadVector(agents::u64 workers,
                                              agents::u64 active = 1,
                                              agents::u64 open_cursors = 1) {
  agents::WorkloadResourceVector vector;
  vector.memory_bytes = 128 * active;
  vector.worker_slots = workers;
  vector.active_requests = active;
  vector.open_cursors = open_cursors;
  vector.buffer_bytes = 64 * active;
  return vector;
}

agents::WorkloadResourcePoolConfig Pool(
    std::string pool_id,
    agents::WorkloadClass workload_class,
    agents::WorkloadResourceVector hard,
    agents::WorkloadResourceVector soft,
    bool queue_on_soft_limit,
    agents::u64 max_queued_requests) {
  agents::WorkloadResourcePoolConfig pool;
  pool.pool_id = std::move(pool_id);
  pool.workload_class = workload_class;
  pool.limits.hard = hard;
  pool.limits.soft = soft;
  pool.limits.queue_on_soft_limit = queue_on_soft_limit;
  pool.limits.max_queued_requests = max_queued_requests;
  pool.limits.maintenance_override_allowed = false;
  return pool;
}

agents::WorkloadAdmissionRequest WorkloadRequest(
    std::string request_uuid,
    std::string pool_id,
    agents::WorkloadClass workload_class,
    agents::WorkloadAdmissionSource source,
    agents::WorkloadResourceVector requested) {
  agents::WorkloadAdmissionRequest request;
  request.request_uuid = std::move(request_uuid);
  request.pool_id = std::move(pool_id);
  request.workload_class = workload_class;
  request.source = source;
  request.requested = requested;
  request.principal_tag = "redacted-orh124";
  return request;
}

agents::WorkloadResourceQuotaController QuotaController() {
  agents::WorkloadResourceQuotaController controller;
  Require(controller.RegisterPool(Pool("foreground.client.dml",
                                       agents::WorkloadClass::foreground,
                                       WorkloadVector(4, 4, 4),
                                       WorkloadVector(3, 3, 3),
                                       false,
                                       0)).ok,
          "foreground pool registration failed");
  Require(controller.RegisterPool(Pool("background.optimized.queue",
                                       agents::WorkloadClass::background,
                                       WorkloadVector(3, 4, 4),
                                       WorkloadVector(1, 4, 4),
                                       true,
                                       1)).ok,
          "queued background pool registration failed");
  Require(controller.RegisterPool(Pool("background.optimized.throttle",
                                       agents::WorkloadClass::background,
                                       WorkloadVector(3, 4, 4),
                                       WorkloadVector(1, 4, 4),
                                       false,
                                       0)).ok,
          "throttled background pool registration failed");
  Require(controller.RegisterPool(Pool("background.optimized.reject",
                                       agents::WorkloadClass::background,
                                       WorkloadVector(1, 1),
                                       WorkloadVector(1, 1),
                                       false,
                                       0)).ok,
          "reject background pool registration failed");
  return controller;
}

agents::OptimizedPathResourceGovernanceRequest GovernRequest(
    std::string operation_id,
    agents::OptimizedPathResourceSurface surface,
    agents::ResourceGovernanceFamily family,
    agents::WorkloadResourceQuotaController* controller,
    std::string pool_id,
    agents::WorkloadClass workload_class,
    agents::WorkloadAdmissionSource source) {
  agents::OptimizedPathResourceGovernanceRequest request;
  request.operation_id = operation_id;
  request.surface = surface;
  request.resource_admission = GovernanceRequest(operation_id, family);
  request.workload_quota = controller;
  request.workload_admission =
      WorkloadRequest(operation_id + ".workload",
                      std::move(pool_id),
                      workload_class,
                      source,
                      WorkloadVector(1));
  request.foreground_pool_id = "foreground.client.dml";
  request.background_workload =
      workload_class == agents::WorkloadClass::background ||
      workload_class == agents::WorkloadClass::maintenance;
  request.foreground_capacity_reserved = true;
  return request;
}

void ProveAdmissionThrottleQueueRejectAndForegroundProtection() {
  auto controller = QuotaController();

  auto foreground = GovernRequest("orh124.foreground.client",
                                  agents::OptimizedPathResourceSurface::cache,
                                  agents::ResourceGovernanceFamily::kOptimizedCache,
                                  &controller,
                                  "foreground.client.dml",
                                  agents::WorkloadClass::foreground,
                                  agents::WorkloadAdmissionSource::listener);
  foreground.foreground_protection_required = false;
  const auto foreground_result = agents::GovernOptimizedPathResources(foreground);
  Require(foreground_result.admitted && foreground_result.reservation_created,
          "foreground client/DML reservation was not admitted");
  Require(foreground_result.decision == "admitted",
          "foreground admitted decision evidence missing");

  auto first_background = GovernRequest(
      "orh124.background.first",
      agents::OptimizedPathResourceSurface::background_job,
      agents::ResourceGovernanceFamily::kBackgroundJob,
      &controller,
      "background.optimized.queue",
      agents::WorkloadClass::background,
      agents::WorkloadAdmissionSource::manager);
  const auto first_background_result =
      agents::GovernOptimizedPathResources(first_background);
  Require(first_background_result.admitted &&
              first_background_result.foreground_protected,
          "first background job was not admitted with foreground protection");

  auto queued = first_background;
  queued.operation_id = "orh124.background.queued";
  queued.resource_admission.operation_id = queued.operation_id;
  queued.resource_admission.descriptor.descriptor_id =
      queued.operation_id + ".quota";
  queued.workload_admission.request_uuid = queued.operation_id + ".workload";
  const auto queued_result = agents::GovernOptimizedPathResources(queued);
  Require(queued_result.queued && queued_result.decision == "queued",
          std::string("background soft-limit queue outcome missing: ") +
              queued_result.decision + "/" + queued_result.diagnostic_code);
  Require(controller.QueuedRequestCount("background.optimized.queue") == 1,
          "background queue count was not recorded");

  auto queue_full = queued;
  queue_full.operation_id = "orh124.background.queue_full";
  queue_full.resource_admission.operation_id = queue_full.operation_id;
  queue_full.resource_admission.descriptor.descriptor_id =
      queue_full.operation_id + ".quota";
  queue_full.workload_admission.request_uuid =
      queue_full.operation_id + ".workload";
  const auto queue_full_result =
      agents::GovernOptimizedPathResources(queue_full);
  Require(queue_full_result.rejected &&
              queue_full_result.diagnostic_code == "WORKLOAD_RESOURCE.QUEUE_FULL",
          "background queue-full rejection missing");

  auto throttled = GovernRequest(
      "orh124.background.throttled",
      agents::OptimizedPathResourceSurface::vector_maintenance,
      agents::ResourceGovernanceFamily::kOptimizedVectorMaintenance,
      &controller,
      "background.optimized.throttle",
      agents::WorkloadClass::background,
      agents::WorkloadAdmissionSource::manager);
  const auto throttle_seed = agents::GovernOptimizedPathResources(throttled);
  Require(throttle_seed.admitted, "background throttle seed was not admitted");
  throttled.operation_id = "orh124.background.throttled.second";
  throttled.resource_admission.operation_id = throttled.operation_id;
  throttled.resource_admission.descriptor.descriptor_id =
      throttled.operation_id + ".quota";
  throttled.workload_admission.request_uuid =
      throttled.operation_id + ".workload";
  const auto throttled_result = agents::GovernOptimizedPathResources(throttled);
  Require(throttled_result.throttled &&
              throttled_result.diagnostic_code ==
                  "WORKLOAD_RESOURCE.SOFT_THROTTLE",
          "background soft-limit throttle outcome missing");

  auto rejected = GovernRequest(
      "orh124.background.rejected",
      agents::OptimizedPathResourceSurface::nosql_provider,
      agents::ResourceGovernanceFamily::kOptimizedNoSqlProvider,
      &controller,
      "background.optimized.reject",
      agents::WorkloadClass::background,
      agents::WorkloadAdmissionSource::engine);
  rejected.workload_admission.requested = WorkloadVector(2);
  const auto rejected_result = agents::GovernOptimizedPathResources(rejected);
  Require(rejected_result.rejected &&
              rejected_result.diagnostic_code == "WORKLOAD_RESOURCE.HARD_DENIED",
          "hard quota rejection outcome missing");

  auto foreground_violation = GovernRequest(
      "orh124.background.foreground_violation",
      agents::OptimizedPathResourceSurface::background_job,
      agents::ResourceGovernanceFamily::kBackgroundJob,
      &controller,
      "foreground.client.dml",
      agents::WorkloadClass::foreground,
      agents::WorkloadAdmissionSource::engine);
  foreground_violation.background_workload = true;
  foreground_violation.foreground_capacity_reserved = false;
  const auto foreground_violation_result =
      agents::GovernOptimizedPathResources(foreground_violation);
  Require(foreground_violation_result.fail_closed &&
              foreground_violation_result.diagnostic_code ==
                  "ORH_RESOURCE_GOVERNANCE.FOREGROUND_PROTECTION_REFUSED",
          "background use of foreground pool was not fail-closed");

  auto foreground_after_pressure = GovernRequest(
      "orh124.foreground.after_background_pressure",
      agents::OptimizedPathResourceSurface::cache,
      agents::ResourceGovernanceFamily::kOptimizedCache,
      &controller,
      "foreground.client.dml",
      agents::WorkloadClass::foreground,
      agents::WorkloadAdmissionSource::listener);
  foreground_after_pressure.foreground_protection_required = false;
  const auto foreground_after_pressure_result =
      agents::GovernOptimizedPathResources(foreground_after_pressure);
  Require(foreground_after_pressure_result.admitted,
          "foreground path starved behind background pressure");

  std::vector<std::string> decisions = {
      foreground_result.decision,
      queued_result.decision,
      queue_full_result.decision,
      throttled_result.decision,
      rejected_result.decision,
      foreground_violation_result.decision,
  };
  Require(Has(decisions, "admitted") && Has(decisions, "queued") &&
              Has(decisions, "throttled") && Has(decisions, "rejected") &&
              Has(decisions, "fail_closed"),
          "admitted queued throttled rejected fail_closed decisions not all proven");
  Require(Has(foreground_violation_result.evidence,
              "optimized_resource.foreground_protected=false"),
          "foreground-protection refusal evidence missing");
}

agents::BackgroundJobSchedulerStartup BackgroundStartup() {
  agents::BackgroundJobSchedulerStartup startup;
  startup.database_uuid = "orh124-database";
  startup.policy_generation = 124;
  startup.tx2_activation_committed = true;
  startup.startup_admitted = true;
  startup.scheduler_catalog_visible = true;
  startup.cluster_authority_available = false;
  startup.monotonic_now_microseconds = 1000;
  return startup;
}

agents::BackgroundJobDefinition BackgroundJob(std::string job_uuid) {
  agents::BackgroundJobDefinition job;
  job.job_uuid = std::move(job_uuid);
  job.job_type = "orh124_background_resource_job";
  job.database_uuid = "orh124-database";
  job.pool_id = "background.optimized.queue";
  job.workload_class = agents::WorkloadClass::background;
  job.source = agents::WorkloadAdmissionSource::engine;
  job.resource_request = WorkloadVector(1);
  job.not_before_microseconds = 1;
  return job;
}

void ProveBackgroundSchedulerUsesBackgroundQuotaOnly() {
  auto controller = QuotaController();
  agents::DatabaseLocalBackgroundJobScheduler scheduler;
  Require(scheduler.Start(BackgroundStartup()).ok,
          "background scheduler start failed");
  Require(scheduler.RegisterJob(BackgroundJob("orh124-scheduled-background"))
              .ok,
          "background scheduler job registration failed");

  const auto scheduled = scheduler.RunNextDue(&controller, 2000);
  Require(scheduled.admitted(),
          "background scheduler job was not admitted through quota");
  Require(controller.UsageForPool("background.optimized.queue").worker_slots ==
              1,
          "scheduled background job did not consume background quota");

  auto foreground = WorkloadRequest("orh124.scheduler.foreground",
                                    "foreground.client.dml",
                                    agents::WorkloadClass::foreground,
                                    agents::WorkloadAdmissionSource::listener,
                                    WorkloadVector(1));
  const auto foreground_result = controller.Admit(foreground);
  Require(foreground_result.status.ok &&
              foreground_result.reservation_created(),
          "background scheduler consumed foreground reserved capacity");
  Require(controller.UsageForPool("foreground.client.dml").worker_slots == 1,
          "foreground reservation was not independent of scheduler job");
}

void ProveResourceAdmissionFallbackSlowdownAndFailClosed() {
  auto slowdown = GovernanceRequest(
      "orh124.compression.slowdown",
      agents::ResourceGovernanceFamily::kOptimizedCompression,
      agents::ResourceGovernanceAction::kSlowdownDegrade);
  slowdown.requested.memory_bytes = slowdown.descriptor.limits.memory_bytes + 1;
  agents::OptimizedPathResourceGovernanceRequest slowdown_request;
  slowdown_request.operation_id = slowdown.operation_id;
  slowdown_request.surface = agents::OptimizedPathResourceSurface::compression;
  slowdown_request.resource_admission = slowdown;
  slowdown_request.workload_quota_required = false;
  const auto slowdown_result =
      agents::GovernOptimizedPathResources(slowdown_request);
  Require(slowdown_result.throttled && slowdown_result.slowdown,
          "resource-governance slowdown outcome missing");

  auto fallback = GovernanceRequest(
      "orh124.cache.fallback",
      agents::ResourceGovernanceFamily::kOptimizedCache,
      agents::ResourceGovernanceAction::kExactScalarFallback);
  fallback.requested.cache_entries =
      fallback.descriptor.limits.cache_entries + 1;
  agents::OptimizedPathResourceGovernanceRequest fallback_request;
  fallback_request.operation_id = fallback.operation_id;
  fallback_request.surface = agents::OptimizedPathResourceSurface::cache;
  fallback_request.resource_admission = fallback;
  fallback_request.workload_quota_required = false;
  const auto fallback_result =
      agents::GovernOptimizedPathResources(fallback_request);
  Require(fallback_result.exact_fallback &&
              fallback_result.decision == "fallback",
          "resource-governance exact fallback outcome missing");

  auto fail_closed = GovernanceRequest(
      "orh124.nosql.fail_closed",
      agents::ResourceGovernanceFamily::kOptimizedNoSqlProvider);
  fail_closed.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kExecution_PlanEvidence;
  fail_closed.descriptor.source_path_or_label =
      "docs" "/completed-execution-plans/optimizer-runtime-hot-path-operationalization-closure";
  agents::OptimizedPathResourceGovernanceRequest fail_closed_request;
  fail_closed_request.operation_id = fail_closed.operation_id;
  fail_closed_request.surface = agents::OptimizedPathResourceSurface::nosql_provider;
  fail_closed_request.resource_admission = fail_closed;
  fail_closed_request.workload_quota_required = false;
  const auto fail_closed_result =
      agents::GovernOptimizedPathResources(fail_closed_request);
  Require(fail_closed_result.fail_closed &&
              fail_closed_result.diagnostic_code ==
                  "SB_RESOURCE_GOVERNANCE.EXECUTION_PLAN_DESCRIPTOR_REFUSED",
          "execution_plan-backed runtime descriptor was not fail-closed");
}

wire::StreamingCursorState CursorState() {
  wire::StreamingCursorState state;
  state.cursor_id = "orh124-cursor";
  state.plan_result_contract_hash = "result-contract-orh124";
  state.catalog_epoch = 10;
  state.descriptor_epoch = 11;
  state.transaction_snapshot_class = "mga_snapshot";
  state.transaction_uuid = "orh124-tx";
  state.local_transaction_id = 12;
  state.snapshot_visible_through_local_transaction_id = 11;
  state.security_epoch = 13;
  state.redaction_epoch = 14;
  state.route_kind = "ipc";
  state.expiry_deadline_unix_millis = 100000;
  state.client_credit.frame_credit = 0;
  state.client_credit.row_credit = 0;
  state.client_credit.byte_credit = 0;
  state.mga_visibility_or_finality_authority = false;
  state.advisory_metadata_only = true;
  return state;
}

void ProveStreamingCursorCreditBackpressureAndCancellation() {
  wire::StreamingCursorManager manager;
  wire::StreamingCursorOpenRequest open;
  open.state = CursorState();
  open.now_unix_millis = 1000;
  const auto opened = manager.OpenCursor(open);
  Require(opened.ok(), "streaming cursor did not open");
  Require(opened.state.client_credit.backpressure_active,
          "zero cursor credit did not activate backpressure");

  wire::StreamingCursorFetchRequest fetch;
  fetch.expected = wire::StreamingCursorBindingFromState(opened.state);
  fetch.now_unix_millis = 1000;
  const auto backpressure = manager.ValidateFetch(fetch);
  Require(!backpressure.ok() &&
              backpressure.diagnostic.diagnostic_code ==
                  "SB_ORH_STREAMING_CURSOR.BACKPRESSURE",
          "cursor fetch without credit was not backpressured");

  wire::StreamingCursorCreditState credit;
  credit.frame_credit = 1;
  credit.row_credit = 3;
  credit.byte_credit = 100;
  const auto granted = manager.GrantCredit(opened.state.cursor_id, credit);
  Require(granted.ok() && !granted.state.client_credit.backpressure_active,
          "cursor credit grant did not clear backpressure");
  fetch.expected = wire::StreamingCursorBindingFromState(granted.state);
  const auto admitted = manager.ValidateFetch(fetch);
  Require(admitted.ok(), "cursor fetch with credit was not admitted");

  wire::StreamingCursorFrameDelivery delivery;
  delivery.expected = fetch.expected;
  delivery.row_count = 2;
  delivery.byte_count = 50;
  delivery.now_unix_millis = 1000;
  const auto delivered = manager.RecordFrameDelivery(delivery);
  Require(delivered.ok() &&
              delivered.state.client_credit.backpressure_active &&
              delivered.state.frame_sequence == 1,
          "cursor frame delivery did not consume credit and reapply backpressure");

  const auto cancelled = manager.CancelCursor(opened.state.cursor_id);
  Require(cancelled.ok(), "cursor cancellation was not recorded");
  fetch.expected = wire::StreamingCursorBindingFromState(cancelled.state);
  const auto refused_after_cancel = manager.ValidateFetch(fetch);
  Require(!refused_after_cancel.ok() &&
              refused_after_cancel.diagnostic.diagnostic_code ==
                  "SB_ORH_STREAMING_CURSOR.CANCELLED",
          "cancelled cursor fetch was not refused");
}

void ProveCompressionMeasuredFeedbackAndBudgetFallback() {
  auto request = idx::DefaultCompressionPolicyRequest(idx::CompressionFamily::kVectorCode);
  request.vector_rerank = true;
  request.uncompressed_bytes = 16384;
  request.estimated_compressed_bytes = 8192;
  request.measured_feedback.present = true;
  request.measured_feedback.compress_ns_per_byte = 20.0;
  request.measured_feedback.decompress_ns_per_byte = 500.0;
  request.measured_feedback.observed_compression_ratio = 0.80;
  request.measured_feedback.sample_count = 512;
  request.measured_feedback.age_ms = 1000;
  request.measured_feedback.dictionary_miss_rate = 0.0;
  request.measured_feedback.fallback_rate = 0.0;
  const auto decision = idx::EvaluateCompressionPolicy(request);
  Require(decision.cost_source == idx::CompressionCostSource::kMeasuredRuntimeFeedback,
          "compression did not use measured runtime feedback");
  Require(decision.fallback &&
              Has(decision.diagnostics,
                  "SB_ORH_COMPRESSION_FAMILY_THRESHOLD.VECTOR_RERANK_DECOMPRESS_DOMINATES"),
          "compression did not fallback when measured decompression exceeded budget");
}

idx::VectorTrainingRecallLifecycleDecision VectorLifecycleDecision() {
  auto profile =
      idx::DefaultVectorTrainingRecallLifecycleProfile(idx::IndexVectorAlgorithm::hnsw);
  profile.drift.deleted_vector_count = 300;
  profile.drift.live_vector_count = 700;
  profile.drift.tombstone_ratio = 0.30;
  profile.drift.max_tombstone_ratio = 0.20;
  profile.drift.hnsw_graph_age_generations = 10;
  profile.drift.max_hnsw_graph_age_generations = 8;
  profile.drift.adaptive_tuning_expected_sufficient = false;
  return idx::EvaluateVectorTrainingRecallLifecycle(profile);
}

void ProveVectorMaintenanceUsesGovernedBackgroundCapacity() {
  auto controller = QuotaController();
  auto request = GovernRequest(
      "orh124.vector.background",
      agents::OptimizedPathResourceSurface::vector_maintenance,
      agents::ResourceGovernanceFamily::kOptimizedVectorMaintenance,
      &controller,
      "background.optimized.queue",
      agents::WorkloadClass::background,
      agents::WorkloadAdmissionSource::manager);
  const auto governed = agents::GovernOptimizedPathResources(request);
  Require(governed.admitted && governed.foreground_protected,
          "vector maintenance background capacity was not governed");

  agents::VectorMaintenanceJobStore store;
  const auto lifecycle = VectorLifecycleDecision();
  Require(lifecycle.accepted, "vector lifecycle decision was not accepted");
  agents::VectorMaintenanceJobRequest job;
  job.job_uuid = "orh124-vector-job";
  job.database_uuid = "orh124-database";
  job.target_collection_uuid = "orh124-collection";
  job.target_index_uuid = "orh124-index";
  job.provider_generation = 2;
  job.old_training_generation = 3;
  job.new_training_generation = 4;
  job.action_kind = agents::VectorMaintenanceActionFromLifecycle(lifecycle.action);
  job.lifecycle_decision = lifecycle;
  job.total_units = 10;
  job.now_microseconds = 124;
  job.exact_fallback_available = true;
  job.exact_rerank_final_scoring_authority = true;
  job.ann_visibility_authority = false;
  job.ann_finality_authority = false;
  const auto created = agents::CreateVectorMaintenanceJob(&store, job);
  Require(created.ok(), "vector maintenance job was not created");
  Require(Has(created.record.evidence, "vector_maintenance_records_only=true"),
          "vector maintenance did not remain metadata-only");
  Require(Has(created.record.evidence, "ann_visibility_authority=false") &&
              Has(created.record.evidence, "ann_finality_authority=false"),
          "vector maintenance claimed visibility/finality authority");
}

nosql::EngineNoSqlPhysicalProviderContract NoSqlContract() {
  nosql::EngineNoSqlPhysicalProviderContract contract;
  contract.family = nosql::EngineNoSqlProviderFamily::kDocument;
  contract.scope = nosql::EngineNoSqlProviderScope::kLocal;
  contract.provider_id = "nosql.local.document.path_provider";
  contract.fallback_provider_id = "nosql.local.document.shape_dictionary";
  contract.local_provider_available = true;
  contract.exact_fallback_available = true;
  contract.descriptor_visibility.proof_present = true;
  contract.descriptor_visibility.visible_to_snapshot = true;
  contract.descriptor_visibility.descriptor_shape_compatible = true;
  contract.security_redaction.proof_present = true;
  contract.security_redaction.redaction_policy_bound = true;
  contract.security_redaction.security_snapshot_bound = true;
  contract.index_generation.proof_present = true;
  contract.index_generation.visible_to_snapshot = true;
  contract.index_generation.covers_predicate = true;
  contract.index_generation.required_generation = 9;
  contract.index_generation.available_generation = 9;
  contract.index_generation.index_uuid = "orh124-document-path-index-proof";
  contract.policy.proof_present = true;
  contract.policy.allowed = true;
  contract.provider_generation.required = true;
  contract.provider_generation.proof_present = true;
  contract.provider_generation.visible_to_snapshot = true;
  contract.provider_generation.publish_state_bound = true;
  contract.provider_generation.validation_state_bound = true;
  contract.provider_generation.backup_restore_repair_metadata_bound = true;
  contract.provider_generation.support_bundle_evidence_bound = true;
  contract.provider_generation.required_generation = 9;
  contract.provider_generation.available_generation = 9;
  contract.provider_generation.descriptor_epoch = 10;
  contract.provider_generation.security_epoch = 11;
  contract.provider_generation.redaction_epoch = 12;
  contract.provider_generation.catalog_epoch = 13;
  contract.provider_generation.generation_uuid = "orh124-generation";
  contract.provider_generation.provider_id = contract.provider_id;
  contract.provider_generation.database_uuid = "orh124-database";
  contract.provider_generation.collection_uuid = "orh124-collection";
  contract.provider_generation.publish_state = "published";
  contract.provider_generation.validation_state = "validated";
  contract.provider_generation.backup_metadata_ref = "backup/orh124";
  contract.provider_generation.restore_metadata_ref = "restore/orh124";
  contract.provider_generation.repair_metadata_ref = "repair/orh124";
  contract.provider_generation.support_bundle_evidence_id = "support/orh124";
  contract.mga_recheck.proof_present = true;
  contract.mga_recheck.row_mga_recheck_required = true;
  contract.mga_recheck.row_security_recheck_required = true;
  contract.mga_recheck.authority_source = "engine_transaction_inventory";
  return contract;
}

void ProveNoSqlProviderResourceEvidencePreservesIndexBlocker() {
  auto controller = QuotaController();
  auto request = GovernRequest(
      "orh124.nosql.path",
      agents::OptimizedPathResourceSurface::nosql_provider,
      agents::ResourceGovernanceFamily::kOptimizedNoSqlProvider,
      &controller,
      "background.optimized.queue",
      agents::WorkloadClass::background,
      agents::WorkloadAdmissionSource::engine);
  request.index_runtime_dependent = true;
  request.index_runtime_correctness_proven = false;
  request.exact_index_runtime_blocker =
      "SB_ORH_NOSQL_DOCUMENT_PATH_INDEX.INDEX_RUNTIME_UNPROVEN";
  const auto governed = agents::GovernOptimizedPathResources(request);
  Require(governed.admitted && governed.foreground_protected,
          "NoSQL provider work was not governed");
  Require(!governed.index_runtime_closure_claimed &&
              Has(governed.evidence,
                  "optimized_resource.index_runtime_closure_claimed=false") &&
              Has(governed.evidence,
                  "optimized_resource.index_runtime_blocker="
                  "SB_ORH_NOSQL_DOCUMENT_PATH_INDEX.INDEX_RUNTIME_UNPROVEN"),
          "NoSQL resource evidence did not preserve ORH-091 blocker");

  const auto selection =
      nosql::SelectLocalNoSqlPhysicalProvider(NoSqlContract());
  Require(selection.ok && selection.selected,
          "NoSQL local physical provider proof was not selected");
  Require(selection.row_mga_recheck_required &&
              selection.row_security_recheck_required,
          "NoSQL provider did not require foreground row rechecks");
  Require(!selection.provider_transaction_finality_authority &&
              !selection.provider_visibility_authority &&
              !selection.parser_transaction_finality_authority,
          "NoSQL provider claimed transaction or visibility authority");
  Require(!selection.descriptor_scan_selected &&
              !selection.behavior_store_scan_selected,
          "NoSQL provider fell back to descriptor/behavior scan");
}

void ProveEvidenceNamesAllRequiredStates() {
  auto controller = QuotaController();
  auto admitted = GovernRequest(
      "orh124.evidence.admitted",
      agents::OptimizedPathResourceSurface::streaming_cursor,
      agents::ResourceGovernanceFamily::kStreamingCursor,
      &controller,
      "foreground.client.dml",
      agents::WorkloadClass::foreground,
      agents::WorkloadAdmissionSource::listener);
  admitted.foreground_protection_required = false;
  const auto result = agents::GovernOptimizedPathResources(admitted);
  const auto serialized =
      agents::SerializeOptimizedPathResourceGovernanceEvidence(result);
  Require(serialized.find("optimized_resource.admitted=true") !=
              std::string::npos &&
              serialized.find("optimized_resource.throttled=false") !=
                  std::string::npos &&
              serialized.find("optimized_resource.queued=false") !=
                  std::string::npos &&
              serialized.find("optimized_resource.rejected=false") !=
                  std::string::npos &&
              serialized.find("optimized_resource.fail_closed=false") !=
                  std::string::npos &&
              serialized.find("optimized_resource.foreground_protected=true") !=
                  std::string::npos,
          "serialized optimized-resource evidence does not distinguish states");
}

}  // namespace

int main() {
  ProveAdmissionThrottleQueueRejectAndForegroundProtection();
  ProveBackgroundSchedulerUsesBackgroundQuotaOnly();
  ProveResourceAdmissionFallbackSlowdownAndFailClosed();
  ProveStreamingCursorCreditBackpressureAndCancellation();
  ProveCompressionMeasuredFeedbackAndBudgetFallback();
  ProveVectorMaintenanceUsesGovernedBackgroundCapacity();
  ProveNoSqlProviderResourceEvidencePreservesIndexBlocker();
  ProveEvidenceNamesAllRequiredStates();
  return EXIT_SUCCESS;
}
