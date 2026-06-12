// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "parallel_physical_pipeline.hpp"
#include "uuid.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <memory>
#include <mutex>
#include <set>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace exec = scratchbird::engine::executor;
namespace agents = scratchbird::core::agents;
namespace idx = scratchbird::core::index;
namespace platform = scratchbird::core::platform;
namespace uuid = scratchbird::core::uuid;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

platform::Status OkStatus() {
  return {platform::StatusCode::ok, platform::Severity::info,
          platform::Subsystem::engine};
}

platform::Status ErrorStatus() {
  return {platform::StatusCode::diagnostic_invalid_record,
          platform::Severity::error, platform::Subsystem::engine};
}

platform::TypedUuid V7(platform::UuidKind kind,
                       platform::u64 unix_epoch_millis,
                       platform::byte suffix) {
  auto generated = uuid::GenerateCompatibilityUnixTimeV7(unix_epoch_millis);
  Require(generated.ok(), "ODF-103 UUIDv7 generation failed");
  generated.value.bytes[6] = 0x70;
  generated.value.bytes[7] = 0x00;
  generated.value.bytes[8] = 0x80;
  for (std::size_t i = 9; i < generated.value.bytes.size(); ++i) {
    generated.value.bytes[i] = 0xa3;
  }
  generated.value.bytes[15] = suffix;
  auto typed = uuid::MakeTypedUuid(kind, generated.value);
  Require(typed.ok(), "ODF-103 typed UUIDv7 creation failed");
  return typed.value;
}

idx::CandidateSetRow Row(platform::byte suffix,
                         double score,
                         bool exact = true,
                         bool visible = true,
                         bool authorized = true,
                         bool payload = true) {
  idx::CandidateSetRow row;
  row.row_uuid = V7(platform::UuidKind::row, 1710000103000ull, suffix);
  row.score = score;
  row.exact_predicate_match = exact;
  row.mga_visible = visible;
  row.security_authorized = authorized;
  row.exact_payload_available = payload;
  row.source = "odf103";
  return row;
}

bool SameUuid(const platform::TypedUuid& left,
              const platform::TypedUuid& right) {
  return left.kind == right.kind && left.value == right.value;
}

exec::ParallelPhysicalWorkerExecutionResult WorkerResultFromRequest(
    const exec::ParallelPhysicalWorkerExecutionRequest& worker_request) {
  exec::ParallelPhysicalWorkerExecutionResult result;
  result.status = OkStatus();
  result.worker_id = worker_request.lane.worker_id;
  result.snapshot_token_id = worker_request.snapshot.token_id;
  result.snapshot_generation = worker_request.snapshot.snapshot_generation;
  result.fragment_count = worker_request.lane.fragment_count;
  result.byte_count = worker_request.lane.byte_count;
  result.candidate_rows = worker_request.lane.candidate_rows;
  result.worker_thread_id = std::this_thread::get_id();
  result.evidence.push_back("odf103.fixture_provider.worker_executed=true");
  result.evidence.push_back("odf103.fixture_provider.worker_id=" +
                            std::to_string(result.worker_id));
  result.diagnostic = exec::MakeParallelPhysicalPipelineDiagnostic(
      result.status, "odf103_fixture_worker_ok",
      "odf103.fixture.worker.ok", "fixture worker returned candidates");
  return result;
}

bool EvidenceHas(const std::vector<std::string>& evidence,
                 std::string_view token) {
  for (const auto& item : evidence) {
    if (item.find(token) != std::string::npos) {
      return true;
    }
  }
  return false;
}

void RequireEvidenceHygiene(const std::vector<std::string>& evidence) {
  for (const auto& item : evidence) {
    for (const auto forbidden :
         {"docs/", "execution-plans", "findings", "contracts", "references",
          "parser_or_reference_finality_or_visibility_authority=true",
          "write_after_stream_finality_or_recovery_authority=true",
          "worker_claims_transaction_finality=true",
          "worker_claims_visibility_authority=true",
          "worker_claims_security_authority=true",
          "finality_authority=true", "visibility_authority=true",
          "security_authority=true", "publication_authority=true",
          "recovery_authority=true"}) {
      Require(item.find(forbidden) == std::string::npos,
              "ODF-103 evidence leaked forbidden authority or document token");
    }
  }
}

idx::CandidateSetAuthorityContext Authority() {
  idx::CandidateSetAuthorityContext authority;
  authority.engine_mga_authoritative = true;
  authority.security_context_bound = true;
  authority.row_mga_recheck_required = true;
  authority.row_security_recheck_required = true;
  authority.exact_recheck_available = true;
  authority.exact_rerank_source_available = true;
  return authority;
}

exec::ParallelPhysicalSnapshotToken Snapshot() {
  exec::ParallelPhysicalSnapshotToken snapshot;
  snapshot.token_id = "odf103.shared.snapshot";
  snapshot.snapshot_generation = 103;
  snapshot.transaction_number = 1003;
  snapshot.visibility_high_water_mark = 1002;
  snapshot.catalog_epoch = 11;
  snapshot.security_epoch = 12;
  snapshot.policy_epoch = 13;
  return snapshot;
}

exec::ParallelPhysicalWorkerLane Lane(
    platform::u32 worker_id,
    const exec::ParallelPhysicalSnapshotToken& snapshot,
    std::vector<idx::CandidateSetRow> rows) {
  exec::ParallelPhysicalWorkerLane lane;
  lane.worker_id = worker_id;
  lane.received_snapshot_token_id = snapshot.token_id;
  lane.received_snapshot_generation = snapshot.snapshot_generation;
  lane.fragment_count = rows.empty() ? 1 : static_cast<platform::u64>(rows.size());
  lane.byte_count = 128 * lane.fragment_count;
  lane.candidate_rows = std::move(rows);
  return lane;
}

std::vector<idx::CandidateSetRow> Rows() {
  return {Row(0x03, 30.0), Row(0x01, 10.0), Row(0x02, 20.0)};
}

exec::ParallelPhysicalPipelineRequest Request(
    exec::ParallelPhysicalPipelineFamily family) {
  const auto snapshot = Snapshot();
  auto rows = Rows();
  exec::ParallelPhysicalPipelineRequest request;
  request.family = family;
  request.snapshot = snapshot;
  request.authority = Authority();
  request.quotas.max_workers = 4;
  request.quotas.max_fragments = 16;
  request.quotas.max_candidate_rows = 16;
  request.quotas.max_bytes = 8192;
  request.worker_lanes = {
      Lane(2, snapshot, {rows[0]}),
      Lane(1, snapshot, {rows[1], rows[2]})};
  request.resource_governance.operation_id = "odf103.parallel_pipeline";
  request.resource_governance.descriptor.descriptor_id =
      "odf106.parallel_pipeline.runtime_quota";
  request.resource_governance.descriptor.family =
      agents::ResourceGovernanceFamily::kParallelPhysicalPipeline;
  request.resource_governance.descriptor.source =
      agents::ResourceGovernanceDescriptorSource::kRuntimePolicy;
  request.resource_governance.descriptor.source_path_or_label =
      "runtime.policy.odf106.parallel_pipeline";
  request.resource_governance.descriptor.descriptor_generation = 103;
  request.resource_governance.descriptor.expected_generation = 103;
  request.resource_governance.descriptor.over_limit_action =
      agents::ResourceGovernanceAction::kSlowdownDegrade;
  request.resource_governance.descriptor.benchmark_clean = true;
  request.resource_governance.descriptor.runtime_dependency_present = true;
  request.resource_governance.descriptor.limits = {
      65536, 1, 1, 8192, 16, 4, 16, 16, 4, 16, 16, 4, 1000000};
  request.resource_governance.requested = {
      8192, 0, 0, 256, 2, 2, 2, 3, 0, 3, 3, 2, 1000};
  return request;
}

void RequireDeterministicFinalRows(
    const exec::ParallelPhysicalPipelineResult& result) {
  Require(result.final_row_uuids.size() == 3,
          "ODF-103 final row count changed");
  Require(result.final_row_uuids[0].value.bytes[15] == 0x01 &&
              result.final_row_uuids[1].value.bytes[15] == 0x02 &&
              result.final_row_uuids[2].value.bytes[15] == 0x03,
          "ODF-103 merge phase did not own deterministic row ordering");
}

void ExpectRefusal(const exec::ParallelPhysicalPipelineRequest& request,
                   std::string_view diagnostic_code,
                   std::string_view message) {
  const auto result = exec::ExecuteParallelPhysicalPipeline(request);
  Require(!result.ok() && result.fail_closed, message);
  Require(result.diagnostic.diagnostic_code == diagnostic_code,
          "ODF-103 refusal diagnostic changed");
  RequireEvidenceHygiene(result.evidence);
}

void AllRequiredFamiliesExecuteAsCandidateOnlyWorkers() {
  const std::vector<exec::ParallelPhysicalPipelineFamily> families = {
      exec::ParallelPhysicalPipelineFamily::kPageScan,
      exec::ParallelPhysicalPipelineFamily::kPageSummaryPrune,
      exec::ParallelPhysicalPipelineFamily::kIndexBuild,
      exec::ParallelPhysicalPipelineFamily::kSearchSegmentBuild,
      exec::ParallelPhysicalPipelineFamily::kVectorExactScanRerank,
      exec::ParallelPhysicalPipelineFamily::kTimeSeriesAggregate,
      exec::ParallelPhysicalPipelineFamily::kGraphFrontierExpansion,
      exec::ParallelPhysicalPipelineFamily::kCopyDecodeBindAppend};

  for (const auto family : families) {
    const auto result = exec::ExecuteParallelPhysicalPipeline(Request(family));
    Require(result.ok(), "ODF-103 required family did not execute");
    Require(result.counters.workers_admitted == 2,
            "ODF-103 worker admission counter changed");
    Require(result.counters.fragments_produced == 3,
            "ODF-103 fragment counter changed");
    Require(result.counters.candidate_rows_produced == 3,
            "ODF-103 candidate row counter changed");
    Require(!result.merged_candidates.output.final_rows_authorized,
            "ODF-103 worker/merge candidate phase authorized final rows");
    Require(result.finalized.recheck.output.final_rows_authorized,
            "ODF-103 final exact recheck did not authorize rows");
    RequireDeterministicFinalRows(result);
    Require(EvidenceHas(result.evidence,
                        "parallel_pipeline.shared_snapshot_token.validated=true"),
            "ODF-103 shared snapshot evidence missing");
    Require(EvidenceHas(result.evidence,
                        "parallel_pipeline.workers_candidate_fragments_only=true"),
            "ODF-103 candidate-fragment evidence missing");
    Require(EvidenceHas(result.evidence,
                        "parallel_pipeline.final_rows_authorized_by_merge=true"),
            "ODF-103 merge finalization evidence missing");
    Require(EvidenceHas(result.evidence,
                        "executor.final_result_requires_mga_recheck=true"),
            "ODF-103 ODF-090 MGA recheck evidence missing");
    Require(EvidenceHas(result.evidence,
                        "executor.final_result_requires_security_recheck=true"),
            "ODF-103 ODF-090 security recheck evidence missing");
    Require(EvidenceHas(result.evidence,
                        "parallel_pipeline.actual_worker_threads=true"),
            "ODF-103 actual worker thread evidence missing");
    Require(EvidenceHas(result.evidence,
                        "parallel_pipeline.worker_thread_count=2"),
            "ODF-103 worker thread count evidence missing");
    Require(EvidenceHas(result.evidence, "resource_governance.route=odf106"),
            "ODF-103 ODF-106 governance admission evidence missing");
    Require(result.worker_thread_ids.size() == 2,
            "ODF-103 worker thread ids were not captured");
    Require(result.worker_thread_ids[0] != std::this_thread::get_id() &&
                result.worker_thread_ids[1] != std::this_thread::get_id(),
            "ODF-103 workers executed on main thread");

    if (family == exec::ParallelPhysicalPipelineFamily::kPageScan) {
      Require(result.counters.page_scan_lanes == 2,
              "ODF-103 page scan lane counter missing");
      Require(EvidenceHas(result.evidence,
                          "storage.parallel_pipeline.family=page_scan"),
              "ODF-103 page scan storage route evidence missing");
    } else if (family ==
               exec::ParallelPhysicalPipelineFamily::kPageSummaryPrune) {
      Require(result.counters.page_summary_prune_lanes == 2,
              "ODF-103 page-summary lane counter missing");
      Require(EvidenceHas(result.evidence,
                          "storage.parallel_pipeline.family=page_summary_prune"),
              "ODF-103 page-summary storage route evidence missing");
    } else if (family == exec::ParallelPhysicalPipelineFamily::kIndexBuild) {
      Require(result.counters.index_build_lanes == 2,
              "ODF-103 index-build lane counter missing");
      Require(EvidenceHas(result.evidence,
                          "storage.parallel_pipeline.family=index_build"),
              "ODF-103 index-build storage route evidence missing");
    } else if (family ==
               exec::ParallelPhysicalPipelineFamily::kSearchSegmentBuild) {
      Require(result.counters.search_segment_build_lanes == 2,
              "ODF-103 search lane counter missing");
      Require(EvidenceHas(result.evidence,
                          "search_segment_candidates_only=true"),
              "ODF-103 search route evidence missing");
    } else if (family ==
               exec::ParallelPhysicalPipelineFamily::kVectorExactScanRerank) {
      Require(result.counters.vector_exact_scan_rerank_lanes == 2,
              "ODF-103 vector lane counter missing");
      Require(EvidenceHas(result.evidence,
                          "parallel_pipeline.vector_exact_rerank=true"),
              "ODF-103 vector rerank evidence missing");
    } else if (family ==
               exec::ParallelPhysicalPipelineFamily::kTimeSeriesAggregate) {
      Require(result.counters.time_series_aggregate_lanes == 2,
              "ODF-103 time-series lane counter missing");
      Require(EvidenceHas(result.evidence,
                          "time_series_partial_aggregate_only=true"),
              "ODF-103 time-series route evidence missing");
    } else if (family ==
               exec::ParallelPhysicalPipelineFamily::kGraphFrontierExpansion) {
      Require(result.counters.graph_frontier_expansion_lanes == 2,
              "ODF-103 graph lane counter missing");
      Require(EvidenceHas(result.evidence,
                          "graph_frontier_candidates_only=true"),
              "ODF-103 graph route evidence missing");
    } else if (family ==
               exec::ParallelPhysicalPipelineFamily::kCopyDecodeBindAppend) {
      Require(result.counters.copy_decode_bind_append_lanes == 2,
              "ODF-103 COPY lane counter missing");
      Require(EvidenceHas(result.evidence,
                          "bulk.parallel_pipeline.family=copy_decode_bind_append"),
              "ODF-103 COPY bulk route evidence missing");
    }
    RequireEvidenceHygiene(result.evidence);
  }
}

void ProviderRunsWorkersConcurrentlyOnDistinctThreads() {
  auto request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  struct SharedState {
    std::mutex mutex;
    std::condition_variable cv;
    int active = 0;
    bool release = false;
    bool timed_out = false;
    std::set<std::thread::id> thread_ids;
  };
  auto state = std::make_shared<SharedState>();
  request.worker_provider =
      [state](const exec::ParallelPhysicalWorkerExecutionRequest& worker_request) {
        auto result = WorkerResultFromRequest(worker_request);
        {
          std::unique_lock<std::mutex> lock(state->mutex);
          ++state->active;
          state->thread_ids.insert(std::this_thread::get_id());
          if (state->active >= 2) {
            state->release = true;
            state->cv.notify_all();
          }
          const bool released = state->cv.wait_for(
              lock, std::chrono::seconds(2),
              [&] { return state->release; });
          if (!released) {
            state->timed_out = true;
            result.status = ErrorStatus();
            result.fail_closed = true;
            result.diagnostic = exec::MakeParallelPhysicalPipelineDiagnostic(
                result.status, "odf103_fixture_workers_not_concurrent",
                "odf103.fixture.workers_not_concurrent",
                "workers did not overlap in provider");
          }
        }
        result.evidence.push_back(
            "odf103.fixture_provider.concurrent_barrier_released=true");
        return result;
      };

  const auto result = exec::ExecuteParallelPhysicalPipeline(request);
  Require(result.ok(), "ODF-103 concurrent worker provider route failed");
  Require(!state->timed_out, "ODF-103 worker provider did not overlap");
  Require(state->thread_ids.size() >= 2,
          "ODF-103 provider did not observe distinct worker threads");
  Require(result.counters.actual_worker_threads >= 2,
          "ODF-103 distinct worker thread counter missing");
  Require(EvidenceHas(result.evidence,
                      "odf103.fixture_provider.concurrent_barrier_released=true"),
          "ODF-103 concurrent provider evidence missing");
  RequireEvidenceHygiene(result.evidence);
}

void ProviderResultRefusalsAreExact() {
  auto request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.worker_provider =
      [](const exec::ParallelPhysicalWorkerExecutionRequest& worker_request) {
        auto result = WorkerResultFromRequest(worker_request);
        result.status = ErrorStatus();
        result.fail_closed = true;
        result.diagnostic = exec::MakeParallelPhysicalPipelineDiagnostic(
            result.status, "odf103_fixture_provider_failed",
            "odf103.fixture.provider_failed",
            "fixture provider failure");
        return result;
      };
  ExpectRefusal(request, "odf103_fixture_provider_failed",
                "ODF-103 provider failure was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.worker_provider =
      [](const exec::ParallelPhysicalWorkerExecutionRequest& worker_request) {
        auto result = WorkerResultFromRequest(worker_request);
        ++result.worker_id;
        return result;
      };
  ExpectRefusal(request, "parallel_pipeline_worker_result_mismatch",
                "ODF-103 provider worker-id mismatch was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.worker_provider =
      [](const exec::ParallelPhysicalWorkerExecutionRequest& worker_request) {
        auto result = WorkerResultFromRequest(worker_request);
        result.snapshot_token_id = "other.snapshot";
        return result;
      };
  ExpectRefusal(request, "parallel_pipeline_snapshot_token_mismatch",
                "ODF-103 provider snapshot mismatch was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.worker_provider =
      [](const exec::ParallelPhysicalWorkerExecutionRequest& worker_request) {
        auto result = WorkerResultFromRequest(worker_request);
        result.final_rows_authorized = true;
        return result;
      };
  ExpectRefusal(request, "parallel_pipeline_worker_final_rows_refused",
                "ODF-103 provider final row authorization was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.worker_provider =
      [](const exec::ParallelPhysicalWorkerExecutionRequest& worker_request) {
        auto result = WorkerResultFromRequest(worker_request);
        result.worker_claims_recovery_authority = true;
        return result;
      };
  ExpectRefusal(request, "parallel_pipeline_worker_forbidden_authority",
                "ODF-103 provider recovery authority was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  const auto caller_thread_id = std::this_thread::get_id();
  request.worker_provider =
      [caller_thread_id](
          const exec::ParallelPhysicalWorkerExecutionRequest& worker_request) {
        auto result = WorkerResultFromRequest(worker_request);
        result.worker_thread_id = caller_thread_id;
        return result;
      };
  ExpectRefusal(request, "parallel_pipeline_worker_not_parallel",
                "ODF-103 provider caller-thread identity spoof was accepted");
}

void SnapshotTokenRefusalsAreExact() {
  auto request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.worker_lanes.front().snapshot_token_present = false;
  request.worker_lanes.front().received_snapshot_token_id.clear();
  ExpectRefusal(request, "parallel_pipeline_snapshot_token_missing",
                "ODF-103 missing worker snapshot was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.snapshot.stale = true;
  ExpectRefusal(request, "parallel_pipeline_snapshot_token_stale",
                "ODF-103 stale shared snapshot was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.worker_lanes.front().received_snapshot_token_id = "other.snapshot";
  ExpectRefusal(request, "parallel_pipeline_snapshot_token_mismatch",
                "ODF-103 mismatched worker snapshot was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.snapshot.engine_mga_snapshot = false;
  ExpectRefusal(request, "parallel_pipeline_mga_snapshot_required",
                "ODF-103 non-MGA snapshot was accepted");
}

void WorkerFinalityAndUnsafeAuthorityRefusalsAreExact() {
  auto request = Request(exec::ParallelPhysicalPipelineFamily::kIndexBuild);
  request.worker_lanes.front().worker_claims_transaction_finality = true;
  ExpectRefusal(request, "parallel_pipeline_worker_forbidden_authority",
                "ODF-103 worker transaction finality was accepted");

  const std::vector<
      std::function<void(exec::ParallelPhysicalWorkerLane*)>>
      unsafe_worker_cases = {
          [](exec::ParallelPhysicalWorkerLane* lane) {
            lane->worker_claims_visibility_authority = true;
          },
          [](exec::ParallelPhysicalWorkerLane* lane) {
            lane->worker_claims_security_authority = true;
          },
          [](exec::ParallelPhysicalWorkerLane* lane) {
            lane->worker_claims_publication_authority = true;
          },
          [](exec::ParallelPhysicalWorkerLane* lane) {
            lane->worker_claims_recovery_authority = true;
          },
          [](exec::ParallelPhysicalWorkerLane* lane) {
            lane->parser_or_reference_finality_or_visibility_authority = true;
          },
          [](exec::ParallelPhysicalWorkerLane* lane) {
            lane->write_after_stream_finality_or_recovery_authority = true;
          },
          [](exec::ParallelPhysicalWorkerLane* lane) {
            lane->timestamp_finality_authority = true;
          },
          [](exec::ParallelPhysicalWorkerLane* lane) {
            lane->uuid_ordering_finality_authority = true;
          }};
  for (const auto& unsafe_case : unsafe_worker_cases) {
    request = Request(exec::ParallelPhysicalPipelineFamily::kIndexBuild);
    unsafe_case(&request.worker_lanes.front());
    ExpectRefusal(request, "parallel_pipeline_worker_forbidden_authority",
                  "ODF-103 unsafe worker authority was accepted");
  }

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.authority.row_mga_recheck_required = false;
  ExpectRefusal(request, "SB_CANDIDATE_SET.MGA_RECHECK_REQUIRED",
                "ODF-103 missing final MGA recheck was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.authority.security_context_bound = false;
  ExpectRefusal(request, "SB_CANDIDATE_SET.SECURITY_RECHECK_REQUIRED",
                "ODF-103 missing final security recheck was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.authority.exact_recheck_available = false;
  ExpectRefusal(request, "SB_CANDIDATE_SET.EXACT_RECHECK_REQUIRED",
                "ODF-103 missing final exact recheck was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.authority.parser_or_reference_finality_or_visibility_authority = true;
  ExpectRefusal(request, "SB_CANDIDATE_SET.UNSAFE_AUTHORITY",
                "ODF-103 parser/reference final authority was accepted");
}

void UnsupportedCorruptCancellationPressureAndQuotaRefusalsAreExact() {
  auto request = Request(exec::ParallelPhysicalPipelineFamily::kUnsupported);
  ExpectRefusal(request, "parallel_pipeline_unsupported_family",
                "ODF-103 unsupported family was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.family = static_cast<exec::ParallelPhysicalPipelineFamily>(999);
  ExpectRefusal(request, "parallel_pipeline_unsupported_family",
                "ODF-103 corrupt family enum was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.worker_lanes.front().corrupt_family_descriptor = true;
  ExpectRefusal(request, "parallel_pipeline_corrupt_family_descriptor",
                "ODF-103 corrupt worker family descriptor was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.cancellation.cancel_before_start = true;
  ExpectRefusal(request, "parallel_pipeline_cancelled",
                "ODF-103 pre-start cancellation was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.worker_lanes.front().cancel_requested = true;
  ExpectRefusal(request, "parallel_pipeline_cancelled",
                "ODF-103 worker cancellation was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.worker_lanes.front().pressure_exceeded = true;
  ExpectRefusal(request, "parallel_pipeline_worker_pressure",
                "ODF-103 worker pressure was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.quotas.max_worker_pressure_bytes = 64;
  ExpectRefusal(request, "parallel_pipeline_worker_pressure",
                "ODF-103 worker byte pressure was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.quotas.max_workers = 1;
  ExpectRefusal(request, "parallel_pipeline_max_workers_exceeded",
                "ODF-103 worker quota overflow was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.quotas.max_fragments = 1;
  ExpectRefusal(request, "parallel_pipeline_max_fragments_exceeded",
                "ODF-103 fragment quota overflow was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.quotas.max_candidate_rows = 1;
  ExpectRefusal(request, "parallel_pipeline_max_candidate_rows_exceeded",
                "ODF-103 candidate quota overflow was accepted");

  request = Request(exec::ParallelPhysicalPipelineFamily::kPageScan);
  request.quotas.max_bytes = 64;
  ExpectRefusal(request, "parallel_pipeline_max_bytes_exceeded",
                "ODF-103 byte quota overflow was accepted");
}

void MergeRecheckFiltersAndOrdersFinalRows() {
  auto request = Request(exec::ParallelPhysicalPipelineFamily::kSearchSegmentBuild);
  const auto snapshot = request.snapshot;
  request.worker_lanes = {
      Lane(3, snapshot, {Row(0x08, 8.0, true, true, true),
                         Row(0x04, 4.0, false, true, true)}),
      Lane(1, snapshot, {Row(0x02, 2.0, true, false, true)}),
      Lane(2, snapshot, {Row(0x06, 6.0, true, true, true)})};
  const auto result = exec::ExecuteParallelPhysicalPipeline(request);
  Require(result.ok(), "ODF-103 filtered merge setup failed");
  Require(result.final_row_uuids.size() == 2,
          "ODF-103 final exact recheck did not filter unsafe candidates");
  Require(result.final_row_uuids[0].value.bytes[15] == 0x06 &&
              result.final_row_uuids[1].value.bytes[15] == 0x08,
          "ODF-103 filtered merge order changed");
  Require(EvidenceHas(result.evidence,
                      "exact_recheck.action=predicate_mga_security"),
          "ODF-103 exact recheck evidence missing");
  RequireEvidenceHygiene(result.evidence);
}

}  // namespace

int main() {
  AllRequiredFamiliesExecuteAsCandidateOnlyWorkers();
  ProviderRunsWorkersConcurrentlyOnDistinctThreads();
  ProviderResultRefusalsAreExact();
  SnapshotTokenRefusalsAreExact();
  WorkerFinalityAndUnsafeAuthorityRefusalsAreExact();
  UnsupportedCorruptCancellationPressureAndQuotaRefusalsAreExact();
  MergeRecheckFiltersAndOrdersFinalRows();
  return EXIT_SUCCESS;
}
