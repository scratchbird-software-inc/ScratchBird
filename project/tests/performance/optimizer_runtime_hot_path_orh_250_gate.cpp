// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "parallel_physical_pipeline.hpp"
#include "resource_governance_admission.hpp"
#include "runtime_platform.hpp"
#include "uuid.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace agents = scratchbird::core::agents;
namespace exec = scratchbird::engine::executor;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ORH-250 gate failure: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 std::string_view needle) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.find(needle) != std::string::npos;
  });
}

platform::Status OkStatus() {
  return {platform::StatusCode::ok,
          platform::Severity::info,
          platform::Subsystem::engine};
}

platform::TypedUuid GeneratedUuid(platform::UuidKind kind,
                                  platform::u64 millis,
                                  platform::byte suffix) {
  const auto generated = uuid::GenerateCompatibilityUnixTimeV7(millis);
  Require(generated.ok(), "uuidv7 generation failed");
  auto value = generated.value;
  value.bytes[15] = suffix;
  const auto typed = uuid::MakeTypedUuid(kind, value);
  Require(typed.ok(), "typed uuid creation failed");
  return typed.value;
}

idx::CandidateSetRow Candidate(platform::byte suffix, double score) {
  idx::CandidateSetRow row;
  row.row_uuid = GeneratedUuid(platform::UuidKind::row,
                               1702500000000ull + suffix,
                               suffix);
  row.score = score;
  row.exact_predicate_match = true;
  row.mga_visible = true;
  row.security_authorized = true;
  row.exact_payload_available = true;
  row.source = "orh250.worker";
  return row;
}

idx::CandidateSetAuthorityContext SafeAuthority() {
  idx::CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = true;
  authority.security_context_bound = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  authority.exact_recheck_available = true;
  authority.exact_rerank_source_available = true;
  return authority;
}

agents::ResourceGovernanceQuotaVector Quotas(std::int64_t value) {
  agents::ResourceGovernanceQuotaVector quotas;
  quotas.memory_bytes = value;
  quotas.device_memory_bytes = value;
  quotas.pinned_memory_bytes = value;
  quotas.io_bytes = value;
  quotas.io_ops = value;
  quotas.worker_threads = value;
  quotas.backlog_items = value;
  quotas.candidate_rows = value;
  quotas.cache_entries = value;
  quotas.batch_rows = value;
  quotas.fragments = value;
  quotas.lanes = value;
  quotas.time_budget_microseconds = value;
  return quotas;
}

agents::ResourceGovernanceAdmissionRequest GovernanceRequest(
    std::string operation_id = "orh250.parallel.foreground",
    agents::ResourceGovernanceAction over_limit =
        agents::ResourceGovernanceAction::kFailClosed) {
  agents::ResourceGovernanceAdmissionRequest request;
  request.operation_id = std::move(operation_id);
  request.expected_family =
      agents::ResourceGovernanceFamily::kParallelPhysicalPipeline;
  request.descriptor.descriptor_id = "runtime.parallel.pipeline.orh250";
  request.descriptor.family =
      agents::ResourceGovernanceFamily::kParallelPhysicalPipeline;
  request.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.descriptor.source_path_or_label = "runtime_policy:orh250";
  request.descriptor.descriptor_generation = 25;
  request.descriptor.expected_generation = 25;
  request.descriptor.limits = Quotas(1000000);
  request.descriptor.over_limit_action = over_limit;
  request.descriptor.benchmark_clean = true;
  request.descriptor.runtime_dependency_present = true;
  request.requested = Quotas(2);
  request.requested.memory_bytes = 4096;
  request.requested.io_bytes = 2048;
  request.requested.worker_threads = 2;
  request.requested.candidate_rows = 2;
  request.requested.batch_rows = 8;
  request.requested.fragments = 2;
  request.requested.lanes = 2;
  request.exact_scalar_fallback_available = true;
  return request;
}

exec::ParallelPhysicalSnapshotToken SnapshotToken() {
  exec::ParallelPhysicalSnapshotToken snapshot;
  snapshot.token_id = "orh250-shared-mga-snapshot";
  snapshot.snapshot_generation = 250;
  snapshot.transaction_number = 251;
  snapshot.visibility_high_water_mark = 252;
  snapshot.catalog_epoch = 253;
  snapshot.security_epoch = 254;
  snapshot.policy_epoch = 255;
  snapshot.engine_mga_snapshot = true;
  snapshot.transaction_inventory_bound = true;
  snapshot.catalog_security_policy_epochs_bound = true;
  return snapshot;
}

exec::ParallelPhysicalWorkerLane Lane(platform::u32 worker_id,
                                      platform::byte row_suffix) {
  exec::ParallelPhysicalWorkerLane lane;
  lane.worker_id = worker_id;
  lane.received_snapshot_token_id = "orh250-shared-mga-snapshot";
  lane.received_snapshot_generation = 250;
  lane.fragment_count = 1;
  lane.byte_count = 256;
  lane.candidate_rows = {Candidate(row_suffix, 100.0 - row_suffix)};
  return lane;
}

exec::ParallelPhysicalWorkerProvider RealForegroundWorkerProvider(
    std::string route_label) {
  return [route_label = std::move(route_label)](
             const exec::ParallelPhysicalWorkerExecutionRequest& request) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    exec::ParallelPhysicalWorkerExecutionResult result;
    result.status = OkStatus();
    result.worker_id = request.lane.worker_id;
    result.snapshot_token_id = request.snapshot.token_id;
    result.snapshot_generation = request.snapshot.snapshot_generation;
    result.fragment_count = request.lane.fragment_count;
    result.byte_count = request.lane.byte_count;
    result.candidate_rows = request.lane.candidate_rows;
    result.worker_thread_id = std::this_thread::get_id();
    result.evidence.push_back("orh250.worker.foreground_thread=true");
    result.evidence.push_back("orh250.worker.route_label=" + route_label);
    result.evidence.push_back("orh250.worker.finality_authority=false");
    result.evidence.push_back("orh250.worker.visibility_authority=false");
    result.evidence.push_back("orh250.worker.security_authority=false");
    return result;
  };
}

exec::ParallelPhysicalPipelineRequest Request(
    exec::ParallelPhysicalPipelineFamily family,
    std::string operation_id) {
  exec::ParallelPhysicalPipelineRequest request;
  request.family = family;
  request.snapshot = SnapshotToken();
  request.authority = SafeAuthority();
  request.resource_governance = GovernanceRequest(std::move(operation_id));
  request.quotas.max_workers = 2;
  request.quotas.max_fragments = 4;
  request.quotas.max_candidate_rows = 4;
  request.quotas.max_bytes = 2048;
  request.quotas.max_worker_pressure_bytes = 1024;
  request.worker_lanes = {Lane(1, 1), Lane(2, 2)};
  request.worker_provider = RealForegroundWorkerProvider(
      exec::ParallelPhysicalPipelineFamilyName(family));
  return request;
}

void RequirePositiveParallelResult(
    const exec::ParallelPhysicalPipelineResult& result,
    exec::ParallelPhysicalPipelineFamily family,
    std::string_view label) {
  Require(result.ok(), std::string(label) + " parallel route refused");
  Require(!result.fallback_used,
          std::string(label) + " unexpectedly used fallback");
  Require(result.counters.workers_admitted == 2,
          std::string(label) + " did not admit both workers");
  Require(result.counters.actual_worker_threads >= 2,
          std::string(label) + " did not use multiple worker threads");
  Require(result.final_row_uuids.size() == 2,
          std::string(label) + " did not finalize two rows");
  Require(HasEvidence(result.evidence,
                      "parallel_pipeline.actual_worker_threads=true"),
          std::string(label) + " missing actual worker evidence");
  Require(HasEvidence(result.evidence,
                      "parallel_pipeline.deterministic_row_uuid_ordering=true"),
          std::string(label) + " missing deterministic ordering evidence");
  Require(HasEvidence(result.evidence,
                      "parallel_pipeline.durable_inventory_finality_owner=merge_phase"),
          std::string(label) + " missing MGA finality authority evidence");
  Require(HasEvidence(result.evidence,
                      "executor.final_result_requires_mga_recheck=true"),
          std::string(label) + " missing final MGA recheck evidence");
  Require(HasEvidence(result.evidence,
                      "executor.final_result_requires_security_recheck=true"),
          std::string(label) + " missing final security recheck evidence");
  Require(HasEvidence(result.evidence,
                      "resource_governance.action=admit"),
          std::string(label) + " missing resource-governance admission");
  Require(HasEvidence(result.evidence,
                      "resource_governance.mga_finality_authority=engine_transaction_inventory"),
          std::string(label) + " missing governor MGA authority evidence");
  switch (family) {
    case exec::ParallelPhysicalPipelineFamily::kPageScan:
      Require(result.counters.page_scan_lanes == 2,
              "scan lane counter mismatch");
      break;
    case exec::ParallelPhysicalPipelineFamily::kJoin:
      Require(result.counters.join_lanes == 2, "join lane counter mismatch");
      break;
    case exec::ParallelPhysicalPipelineFamily::kTimeSeriesAggregate:
      Require(result.counters.time_series_aggregate_lanes == 2,
              "aggregate lane counter mismatch");
      break;
    case exec::ParallelPhysicalPipelineFamily::kDmlUpdate:
      Require(result.counters.dml_update_lanes == 2,
              "update lane counter mismatch");
      break;
    case exec::ParallelPhysicalPipelineFamily::kCopyDecodeBindAppend:
      Require(result.counters.copy_decode_bind_append_lanes == 2,
              "bulk ingest lane counter mismatch");
      break;
    default:
      Fail("unexpected positive family");
  }
}

void TestPositiveForegroundWorkersForRequiredFamilies() {
  struct Case {
    exec::ParallelPhysicalPipelineFamily family;
    const char* operation_id;
    const char* label;
    const char* route_evidence;
  };
  const Case cases[] = {
      {exec::ParallelPhysicalPipelineFamily::kPageScan,
       "orh250.scan.foreground",
       "scan",
       "storage.parallel_pipeline.family=page_scan"},
      {exec::ParallelPhysicalPipelineFamily::kJoin,
       "orh250.join.foreground",
       "join",
       "executor.parallel_pipeline.family_route=join"},
      {exec::ParallelPhysicalPipelineFamily::kTimeSeriesAggregate,
       "orh250.aggregate.foreground",
       "aggregate",
       "executor.parallel_pipeline.family_route=time_series_aggregate"},
      {exec::ParallelPhysicalPipelineFamily::kDmlUpdate,
       "orh250.update.foreground",
       "update",
       "executor.parallel_pipeline.family_route=dml_update"},
      {exec::ParallelPhysicalPipelineFamily::kCopyDecodeBindAppend,
       "orh250.bulk_ingest.foreground",
       "bulk_ingest",
       "bulk.parallel_pipeline.family=copy_decode_bind_append"}};

  for (const auto& item : cases) {
    auto request = Request(item.family, item.operation_id);
    const auto result = exec::ExecuteParallelPhysicalPipeline(request);
    RequirePositiveParallelResult(result, item.family, item.label);
    Require(HasEvidence(result.evidence, item.route_evidence),
            std::string(item.label) + " missing route-family evidence");
  }
}

void TestResourceGovernorRefusalAndFallback() {
  auto over_threads =
      Request(exec::ParallelPhysicalPipelineFamily::kPageScan,
              "orh250.negative.over_threads");
  over_threads.resource_governance.descriptor.limits.worker_threads = 1;
  over_threads.resource_governance.requested.worker_threads = 3;
  auto result = exec::ExecuteParallelPhysicalPipeline(over_threads);
  Require(!result.ok() &&
              result.diagnostic.diagnostic_code ==
                  "parallel_pipeline_odf106_quota_refused",
          "over-thread quota did not fail closed");
  Require(HasEvidence(result.evidence,
                      "resource_governance.exceeded_quota=worker_threads"),
          "over-thread evidence missing");

  auto over_memory =
      Request(exec::ParallelPhysicalPipelineFamily::kPageScan,
              "orh250.negative.over_memory");
  over_memory.resource_governance.descriptor.limits.memory_bytes = 1024;
  over_memory.resource_governance.requested.memory_bytes = 4096;
  result = exec::ExecuteParallelPhysicalPipeline(over_memory);
  Require(!result.ok() &&
              HasEvidence(result.evidence,
                          "resource_governance.exceeded_quota=memory_bytes"),
          "over-memory quota did not fail closed");

  auto fallback =
      Request(exec::ParallelPhysicalPipelineFamily::kPageScan,
              "orh250.fallback.workers_unavailable");
  fallback.resource_governance.descriptor.over_limit_action =
      agents::ResourceGovernanceAction::kExactScalarFallback;
  fallback.resource_governance.descriptor.limits.worker_threads = 1;
  fallback.resource_governance.requested.worker_threads = 2;
  fallback.resource_governance.require_exact_scalar_fallback_available = true;
  fallback.resource_governance.exact_scalar_fallback_available = true;
  result = exec::ExecuteParallelPhysicalPipeline(fallback);
  Require(result.ok() && result.fallback_used,
          "worker-unavailable route did not use exact fallback");
  Require(HasEvidence(result.evidence,
                      "parallel_pipeline.fallback_reason=odf106_quota"),
          "worker-unavailable fallback evidence missing");
  Require(HasEvidence(result.evidence,
                      "resource_governance.action=exact_scalar_fallback"),
          "exact fallback governor action missing");
}

void TestCancellationAndAuthorityRefusals() {
  auto cancel_governor =
      Request(exec::ParallelPhysicalPipelineFamily::kPageScan,
              "orh250.cancel.before.governor");
  cancel_governor.resource_governance.cancellation_requested = true;
  auto result = exec::ExecuteParallelPhysicalPipeline(cancel_governor);
  Require(!result.ok() &&
              HasEvidence(result.evidence,
                          "resource_governance.action=cancel"),
          "governor cancellation before start was not refused");

  auto cancel_before =
      Request(exec::ParallelPhysicalPipelineFamily::kPageScan,
              "orh250.cancel.before.pipeline");
  cancel_before.cancellation.cancel_before_start = true;
  result = exec::ExecuteParallelPhysicalPipeline(cancel_before);
  Require(!result.ok() &&
              result.diagnostic.diagnostic_code ==
                  "parallel_pipeline_cancelled",
          "pipeline cancellation before workers was not refused");

  auto cancel_during =
      Request(exec::ParallelPhysicalPipelineFamily::kPageScan,
              "orh250.cancel.during.worker");
  cancel_during.worker_provider =
      [](const exec::ParallelPhysicalWorkerExecutionRequest& request) {
        auto provider = RealForegroundWorkerProvider("cancel_during_execution");
        auto result = provider(request);
        if (request.lane.worker_id == 2) {
          result.cancel_requested = true;
        }
        return result;
      };
  result = exec::ExecuteParallelPhysicalPipeline(cancel_during);
  Require(!result.ok() &&
              result.diagnostic.diagnostic_code ==
                  "parallel_pipeline_cancelled",
          "worker-lane cancellation during execution was not refused");

  auto missing_snapshot =
      Request(exec::ParallelPhysicalPipelineFamily::kPageScan,
              "orh250.negative.missing_snapshot");
  missing_snapshot.snapshot.engine_mga_snapshot = false;
  result = exec::ExecuteParallelPhysicalPipeline(missing_snapshot);
  Require(!result.ok() &&
              result.diagnostic.diagnostic_code ==
                  "parallel_pipeline_mga_snapshot_required",
          "missing MGA snapshot proof was not refused");

  auto parser_donor =
      Request(exec::ParallelPhysicalPipelineFamily::kPageScan,
              "orh250.negative.parser_donor");
  parser_donor.worker_lanes[0].parser_or_donor_finality_or_visibility_authority =
      true;
  result = exec::ExecuteParallelPhysicalPipeline(parser_donor);
  Require(!result.ok() &&
              result.diagnostic.diagnostic_code ==
                  "parallel_pipeline_worker_forbidden_authority",
          "parser/donor worker authority was not refused");

  auto client_authority =
      Request(exec::ParallelPhysicalPipelineFamily::kPageScan,
              "orh250.negative.client_authority");
  client_authority.authority.client_finality_or_visibility_authority = true;
  result = exec::ExecuteParallelPhysicalPipeline(client_authority);
  Require(!result.ok() &&
              HasEvidence(result.evidence,
                          "parallel_pipeline.fail_closed=true"),
          "client finality/visibility authority was not refused");

  auto worker_final_rows =
      Request(exec::ParallelPhysicalPipelineFamily::kPageScan,
              "orh250.negative.worker_final_rows");
  worker_final_rows.worker_provider =
      [](const exec::ParallelPhysicalWorkerExecutionRequest& request) {
        auto provider = RealForegroundWorkerProvider("unsafe_final_rows");
        auto result = provider(request);
        result.final_rows_authorized = true;
        return result;
      };
  result = exec::ExecuteParallelPhysicalPipeline(worker_final_rows);
  Require(!result.ok() &&
              result.diagnostic.diagnostic_code ==
                  "parallel_pipeline_worker_final_rows_refused",
          "worker-authorized final rows were not refused");
}

}  // namespace

int main() {
  TestPositiveForegroundWorkersForRequiredFamilies();
  TestResourceGovernorRefusalAndFallback();
  TestCancellationAndAuthorityRefusals();
  return EXIT_SUCCESS;
}
