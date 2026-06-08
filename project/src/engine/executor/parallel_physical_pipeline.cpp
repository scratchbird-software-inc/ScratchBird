// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "parallel_physical_pipeline.hpp"

#include "parallel_copy_pipeline_route.hpp"
#include "parallel_physical_storage_routes.hpp"

#include <algorithm>
#include <future>
#include <limits>
#include <set>
#include <sstream>
#include <thread>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

using scratchbird::core::index::CandidateSet;
using scratchbird::core::index::CandidateSetAuthorityContext;
using scratchbird::core::index::CandidateSetRow;
using scratchbird::core::index::MakeExactRowUuidOrderedCandidateSet;
using scratchbird::core::index::RerankCandidateSet;
using scratchbird::core::platform::Severity;
using scratchbird::core::platform::StatusCode;
using scratchbird::core::platform::Subsystem;
namespace agents = scratchbird::core::agents;

Status OkStatus() {
  return {StatusCode::ok, Severity::info, Subsystem::engine};
}

Status RefusalStatus() {
  return {StatusCode::diagnostic_invalid_record, Severity::error,
          Subsystem::engine};
}

bool FamilySupported(ParallelPhysicalPipelineFamily family) {
  switch (family) {
    case ParallelPhysicalPipelineFamily::kPageScan:
    case ParallelPhysicalPipelineFamily::kPageSummaryPrune:
    case ParallelPhysicalPipelineFamily::kIndexBuild:
    case ParallelPhysicalPipelineFamily::kSearchSegmentBuild:
    case ParallelPhysicalPipelineFamily::kVectorExactScanRerank:
    case ParallelPhysicalPipelineFamily::kTimeSeriesAggregate:
    case ParallelPhysicalPipelineFamily::kGraphFrontierExpansion:
    case ParallelPhysicalPipelineFamily::kCopyDecodeBindAppend:
    case ParallelPhysicalPipelineFamily::kJoin:
    case ParallelPhysicalPipelineFamily::kDmlUpdate:
      return true;
    case ParallelPhysicalPipelineFamily::kUnsupported:
      return false;
  }
  return false;
}

std::vector<std::string> BaseEvidence(
    ParallelPhysicalPipelineFamily family,
    const ParallelPhysicalSnapshotToken& snapshot) {
  return {"parallel_pipeline.family=" +
              std::string(ParallelPhysicalPipelineFamilyName(family)),
          "parallel_pipeline.route_generation=103",
          "parallel_pipeline.bounded_worker_lanes=true",
          "parallel_pipeline.shared_snapshot_token=" + snapshot.token_id,
          "parallel_pipeline.shared_snapshot_generation=" +
              std::to_string(snapshot.snapshot_generation),
          "parallel_pipeline.workers_candidate_fragments_only=true",
          "parallel_pipeline.worker_finality_authority=false",
          "parallel_pipeline.merge_owns_result_ordering=true",
          "parallel_pipeline.final_mga_security_exact_recheck_required=true",
          "parallel_pipeline.odf090_candidate_set_recheck=true",
          "parallel_pipeline.durable_inventory_finality_owner=merge_phase"};
}

void AppendEvidence(std::vector<std::string>* target,
                    const std::vector<std::string>& source) {
  target->insert(target->end(), source.begin(), source.end());
}

void AppendGovernanceEvidence(
    ParallelPhysicalPipelineResult* result,
    const agents::ResourceGovernanceAdmissionResult& governance) {
  AppendEvidence(&result->evidence, governance.evidence);
  result->evidence.push_back("parallel_pipeline.resource_governance_action=" +
                             std::string(agents::ResourceGovernanceActionName(
                                 governance.action)));
}

ParallelPhysicalPipelineResult Refuse(
    const ParallelPhysicalPipelineRequest& request,
    std::string diagnostic_code,
    std::string message_key,
    std::string reason) {
  ParallelPhysicalPipelineResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.diagnostic = MakeParallelPhysicalPipelineDiagnostic(
      result.status, std::move(diagnostic_code), std::move(message_key),
      reason);
  result.refusal_reasons.push_back(reason);
  result.evidence = BaseEvidence(request.family, request.snapshot);
  result.evidence.push_back("parallel_pipeline.fail_closed=true");
  result.evidence.push_back("parallel_pipeline.refusal_reason=" + reason);
  return result;
}

ParallelPhysicalPipelineResult GovernanceRefuse(
    const ParallelPhysicalPipelineRequest& request,
    const agents::ResourceGovernanceAdmissionResult& governance) {
  auto result = Refuse(request, "parallel_pipeline_odf106_quota_refused",
                       "executor.parallel_pipeline.odf106_quota_refused",
                       governance.diagnostic_code);
  AppendGovernanceEvidence(&result, governance);
  return result;
}

ParallelPhysicalPipelineResult GovernanceFallback(
    const ParallelPhysicalPipelineRequest& request,
    const agents::ResourceGovernanceAdmissionResult& governance) {
  ParallelPhysicalPipelineResult result;
  result.status = OkStatus();
  result.fallback_used = true;
  result.evidence = BaseEvidence(request.family, request.snapshot);
  result.evidence.push_back("parallel_pipeline.fallback_used=true");
  result.evidence.push_back("parallel_pipeline.fallback_reason=odf106_quota");
  result.diagnostic = MakeParallelPhysicalPipelineDiagnostic(
      result.status, "parallel_pipeline_odf106_quota_degrade",
      "executor.parallel_pipeline.odf106_quota_degrade",
      governance.diagnostic_code);
  AppendGovernanceEvidence(&result, governance);
  return result;
}

bool SnapshotTokenPresent(const ParallelPhysicalSnapshotToken& snapshot) {
  return !snapshot.token_id.empty() && snapshot.snapshot_generation != 0 &&
         snapshot.transaction_number != 0;
}

ParallelPhysicalPipelineResult ValidateSnapshot(
    const ParallelPhysicalPipelineRequest& request) {
  if (!SnapshotTokenPresent(request.snapshot)) {
    return Refuse(request, "parallel_pipeline_snapshot_token_missing",
                  "executor.parallel_pipeline.snapshot_token_missing",
                  "missing_shared_mga_snapshot_token");
  }
  if (request.snapshot.stale) {
    return Refuse(request, "parallel_pipeline_snapshot_token_stale",
                  "executor.parallel_pipeline.snapshot_token_stale",
                  "stale_shared_mga_snapshot_token");
  }
  if (!request.snapshot.engine_mga_snapshot ||
      !request.snapshot.transaction_inventory_bound) {
    return Refuse(request, "parallel_pipeline_mga_snapshot_required",
                  "executor.parallel_pipeline.mga_snapshot_required",
                  "shared_snapshot_not_bound_to_mga_transaction_inventory");
  }
  if (!request.snapshot.catalog_security_policy_epochs_bound ||
      request.snapshot.catalog_epoch == 0 || request.snapshot.security_epoch == 0 ||
      request.snapshot.policy_epoch == 0) {
    return Refuse(request, "parallel_pipeline_snapshot_epoch_binding_required",
                  "executor.parallel_pipeline.snapshot_epoch_binding_required",
                  "snapshot_missing_catalog_security_policy_epoch_binding");
  }
  ParallelPhysicalPipelineResult ok;
  ok.status = OkStatus();
  return ok;
}

bool WorkerClaimsForbiddenAuthority(
    const ParallelPhysicalWorkerLane& lane) {
  return lane.worker_claims_transaction_finality ||
         lane.worker_claims_visibility_authority ||
         lane.worker_claims_security_authority ||
         lane.worker_claims_publication_authority ||
         lane.worker_claims_recovery_authority ||
         lane.parser_or_donor_finality_or_visibility_authority ||
         lane.write_after_stream_finality_or_recovery_authority ||
         lane.timestamp_finality_authority ||
         lane.uuid_ordering_finality_authority;
}

bool WorkerResultClaimsForbiddenAuthority(
    const ParallelPhysicalWorkerExecutionResult& worker_result) {
  return worker_result.worker_claims_transaction_finality ||
         worker_result.worker_claims_visibility_authority ||
         worker_result.worker_claims_security_authority ||
         worker_result.worker_claims_publication_authority ||
         worker_result.worker_claims_recovery_authority ||
         worker_result.parser_or_donor_finality_or_visibility_authority ||
         worker_result.write_after_stream_finality_or_recovery_authority ||
         worker_result.timestamp_finality_authority ||
         worker_result.uuid_ordering_finality_authority;
}

u64 LaneFragmentCount(const ParallelPhysicalWorkerLane& lane) {
  if (lane.fragment_count != 0) {
    return lane.fragment_count;
  }
  if (!lane.candidate_rows.empty()) {
    return static_cast<u64>(lane.candidate_rows.size());
  }
  return 1;
}

u64 ResultFragmentCount(
    const ParallelPhysicalWorkerExecutionResult& worker_result) {
  if (worker_result.fragment_count != 0) {
    return worker_result.fragment_count;
  }
  if (!worker_result.candidate_rows.empty()) {
    return static_cast<u64>(worker_result.candidate_rows.size());
  }
  return 1;
}

std::string ThreadIdString(std::thread::id thread_id) {
  std::ostringstream out;
  out << thread_id;
  return out.str();
}

bool AddWouldOverflow(u64 left, u64 right) {
  return std::numeric_limits<u64>::max() - left < right;
}

bool AddWithinQuota(u64* current, u64 value, u64 quota) {
  if (current == nullptr || AddWouldOverflow(*current, value)) {
    return false;
  }
  *current += value;
  return *current <= quota;
}

void IncrementFamilyLane(ParallelPhysicalPipelineFamily family,
                         ParallelPhysicalPipelineCounters* counters) {
  if (counters == nullptr) {
    return;
  }
  switch (family) {
    case ParallelPhysicalPipelineFamily::kPageScan:
      ++counters->page_scan_lanes;
      break;
    case ParallelPhysicalPipelineFamily::kPageSummaryPrune:
      ++counters->page_summary_prune_lanes;
      break;
    case ParallelPhysicalPipelineFamily::kIndexBuild:
      ++counters->index_build_lanes;
      break;
    case ParallelPhysicalPipelineFamily::kSearchSegmentBuild:
      ++counters->search_segment_build_lanes;
      break;
    case ParallelPhysicalPipelineFamily::kVectorExactScanRerank:
      ++counters->vector_exact_scan_rerank_lanes;
      break;
    case ParallelPhysicalPipelineFamily::kTimeSeriesAggregate:
      ++counters->time_series_aggregate_lanes;
      break;
    case ParallelPhysicalPipelineFamily::kGraphFrontierExpansion:
      ++counters->graph_frontier_expansion_lanes;
      break;
    case ParallelPhysicalPipelineFamily::kCopyDecodeBindAppend:
      ++counters->copy_decode_bind_append_lanes;
      break;
    case ParallelPhysicalPipelineFamily::kJoin:
      ++counters->join_lanes;
      break;
    case ParallelPhysicalPipelineFamily::kDmlUpdate:
      ++counters->dml_update_lanes;
      break;
    case ParallelPhysicalPipelineFamily::kUnsupported:
      break;
  }
}

void AppendRouteEvidence(ParallelPhysicalPipelineFamily family,
                         std::vector<std::string>* evidence) {
  switch (family) {
    case ParallelPhysicalPipelineFamily::kPageScan: {
      const auto route =
          scratchbird::storage::page::BuildParallelStoragePipelineRouteEvidence(
              scratchbird::storage::page::ParallelStoragePipelineRouteFamily::
                  kPageScan);
      AppendEvidence(evidence, route.evidence);
      break;
    }
    case ParallelPhysicalPipelineFamily::kPageSummaryPrune: {
      const auto route =
          scratchbird::storage::page::BuildParallelStoragePipelineRouteEvidence(
              scratchbird::storage::page::ParallelStoragePipelineRouteFamily::
                  kPageSummaryPrune);
      AppendEvidence(evidence, route.evidence);
      break;
    }
    case ParallelPhysicalPipelineFamily::kIndexBuild: {
      const auto route =
          scratchbird::storage::page::BuildParallelStoragePipelineRouteEvidence(
              scratchbird::storage::page::ParallelStoragePipelineRouteFamily::
                  kIndexBuild);
      AppendEvidence(evidence, route.evidence);
      break;
    }
    case ParallelPhysicalPipelineFamily::kCopyDecodeBindAppend: {
      const auto route =
          scratchbird::core::bulk_load::BuildParallelCopyPipelineRouteEvidence();
      AppendEvidence(evidence, route.evidence);
      break;
    }
    case ParallelPhysicalPipelineFamily::kSearchSegmentBuild:
      evidence->push_back(
          "executor.parallel_pipeline.family_route=search_segment_build");
      evidence->push_back(
          "executor.parallel_pipeline.search_segment_candidates_only=true");
      break;
    case ParallelPhysicalPipelineFamily::kVectorExactScanRerank:
      evidence->push_back(
          "executor.parallel_pipeline.family_route=vector_exact_scan_rerank");
      evidence->push_back("parallel_pipeline.vector_exact_rerank=true");
      break;
    case ParallelPhysicalPipelineFamily::kTimeSeriesAggregate:
      evidence->push_back(
          "executor.parallel_pipeline.family_route=time_series_aggregate");
      evidence->push_back(
          "executor.parallel_pipeline.time_series_partial_aggregate_only=true");
      break;
    case ParallelPhysicalPipelineFamily::kGraphFrontierExpansion:
      evidence->push_back(
          "executor.parallel_pipeline.family_route=graph_frontier_expansion");
      evidence->push_back(
          "executor.parallel_pipeline.graph_frontier_candidates_only=true");
      break;
    case ParallelPhysicalPipelineFamily::kJoin:
      evidence->push_back("executor.parallel_pipeline.family_route=join");
      evidence->push_back(
          "executor.parallel_pipeline.join_candidate_fragments_only=true");
      break;
    case ParallelPhysicalPipelineFamily::kDmlUpdate:
      evidence->push_back("executor.parallel_pipeline.family_route=dml_update");
      evidence->push_back(
          "executor.parallel_pipeline.dml_update_locator_fragments_only=true");
      break;
    case ParallelPhysicalPipelineFamily::kUnsupported:
      break;
  }
}

std::string WorkerRouteDescriptor(ParallelPhysicalPipelineFamily family,
                                  u32 worker_id,
                                  const std::string& snapshot_token_id,
                                  std::vector<std::string>* evidence) {
  switch (family) {
    case ParallelPhysicalPipelineFamily::kPageScan: {
      const auto route = scratchbird::storage::page::
          BuildParallelStoragePipelineWorkerRouteDescriptor(
              scratchbird::storage::page::ParallelStoragePipelineRouteFamily::
                  kPageScan,
              worker_id, snapshot_token_id);
      AppendEvidence(evidence, route.evidence);
      return route.worker_route_descriptor;
    }
    case ParallelPhysicalPipelineFamily::kPageSummaryPrune: {
      const auto route = scratchbird::storage::page::
          BuildParallelStoragePipelineWorkerRouteDescriptor(
              scratchbird::storage::page::ParallelStoragePipelineRouteFamily::
                  kPageSummaryPrune,
              worker_id, snapshot_token_id);
      AppendEvidence(evidence, route.evidence);
      return route.worker_route_descriptor;
    }
    case ParallelPhysicalPipelineFamily::kIndexBuild: {
      const auto route = scratchbird::storage::page::
          BuildParallelStoragePipelineWorkerRouteDescriptor(
              scratchbird::storage::page::ParallelStoragePipelineRouteFamily::
                  kIndexBuild,
              worker_id, snapshot_token_id);
      AppendEvidence(evidence, route.evidence);
      return route.worker_route_descriptor;
    }
    case ParallelPhysicalPipelineFamily::kCopyDecodeBindAppend: {
      const auto route =
          scratchbird::core::bulk_load::
              BuildParallelCopyPipelineWorkerRouteDescriptor(worker_id,
                                                             snapshot_token_id);
      AppendEvidence(evidence, route.evidence);
      return route.worker_route_descriptor;
    }
    case ParallelPhysicalPipelineFamily::kSearchSegmentBuild:
      evidence->push_back("executor.parallel_worker.worker_id=" +
                          std::to_string(worker_id));
      evidence->push_back(
          "executor.parallel_worker.route_descriptor=executor.search_segment_build");
      return "executor.parallel_worker.search_segment_build";
    case ParallelPhysicalPipelineFamily::kVectorExactScanRerank:
      evidence->push_back("executor.parallel_worker.worker_id=" +
                          std::to_string(worker_id));
      evidence->push_back(
          "executor.parallel_worker.route_descriptor=executor.vector_exact_scan_rerank");
      return "executor.parallel_worker.vector_exact_scan_rerank";
    case ParallelPhysicalPipelineFamily::kTimeSeriesAggregate:
      evidence->push_back("executor.parallel_worker.worker_id=" +
                          std::to_string(worker_id));
      evidence->push_back(
          "executor.parallel_worker.route_descriptor=executor.time_series_aggregate");
      return "executor.parallel_worker.time_series_aggregate";
    case ParallelPhysicalPipelineFamily::kGraphFrontierExpansion:
      evidence->push_back("executor.parallel_worker.worker_id=" +
                          std::to_string(worker_id));
      evidence->push_back(
          "executor.parallel_worker.route_descriptor=executor.graph_frontier_expansion");
      return "executor.parallel_worker.graph_frontier_expansion";
    case ParallelPhysicalPipelineFamily::kJoin:
      evidence->push_back("executor.parallel_worker.worker_id=" +
                          std::to_string(worker_id));
      evidence->push_back(
          "executor.parallel_worker.route_descriptor=executor.join");
      return "executor.parallel_worker.join";
    case ParallelPhysicalPipelineFamily::kDmlUpdate:
      evidence->push_back("executor.parallel_worker.worker_id=" +
                          std::to_string(worker_id));
      evidence->push_back(
          "executor.parallel_worker.route_descriptor=executor.dml_update");
      return "executor.parallel_worker.dml_update";
    case ParallelPhysicalPipelineFamily::kUnsupported:
      break;
  }
  return {};
}

bool RowUuidLess(const CandidateSetRow& left, const CandidateSetRow& right) {
  if (left.row_uuid.kind != right.row_uuid.kind) {
    return static_cast<u32>(left.row_uuid.kind) <
           static_cast<u32>(right.row_uuid.kind);
  }
  return left.row_uuid.value.bytes < right.row_uuid.value.bytes;
}

ParallelPhysicalWorkerExecutionResult DefaultParallelPhysicalWorkerProvider(
    const ParallelPhysicalWorkerExecutionRequest& worker_request) {
  ParallelPhysicalWorkerExecutionResult result;
  result.status = OkStatus();
  result.worker_id = worker_request.lane.worker_id;
  result.snapshot_token_id = worker_request.snapshot.token_id;
  result.snapshot_generation = worker_request.snapshot.snapshot_generation;
  result.fragment_count = LaneFragmentCount(worker_request.lane);
  result.byte_count = worker_request.lane.byte_count;
  result.candidate_rows = worker_request.lane.candidate_rows;
  result.worker_thread_id = std::this_thread::get_id();
  result.evidence.push_back("parallel_worker.worker_id=" +
                            std::to_string(result.worker_id));
  result.evidence.push_back("parallel_worker.thread_id=" +
                            ThreadIdString(result.worker_thread_id));
  result.evidence.push_back("parallel_worker.route_descriptor=" +
                            worker_request.worker_route_descriptor);
  result.evidence.push_back(
      "parallel_worker.produced_candidate_fragments_only=true");
  result.diagnostic = MakeParallelPhysicalPipelineDiagnostic(
      result.status, "parallel_worker_ok",
      "executor.parallel_pipeline.worker.executed",
      "parallel worker returned candidate fragments");
  return result;
}

ParallelPhysicalPipelineResult ValidateLaneDescriptor(
    const ParallelPhysicalPipelineRequest& request,
    const ParallelPhysicalWorkerLane& lane,
    std::set<u32>* worker_ids) {
  if (worker_ids != nullptr && !worker_ids->insert(lane.worker_id).second) {
    return Refuse(request, "parallel_pipeline_duplicate_worker",
                  "executor.parallel_pipeline.duplicate_worker",
                  "duplicate_worker_lane_id");
  }
  if (lane.corrupt_family_descriptor) {
    return Refuse(request, "parallel_pipeline_corrupt_family_descriptor",
                  "executor.parallel_pipeline.corrupt_family_descriptor",
                  "corrupt_worker_family_descriptor");
  }
  if (!lane.snapshot_token_present || lane.received_snapshot_token_id.empty()) {
    return Refuse(request, "parallel_pipeline_snapshot_token_missing",
                  "executor.parallel_pipeline.snapshot_token_missing",
                  "worker_missing_shared_mga_snapshot_token");
  }
  if (lane.received_snapshot_stale) {
    return Refuse(request, "parallel_pipeline_snapshot_token_stale",
                  "executor.parallel_pipeline.snapshot_token_stale",
                  "worker_received_stale_mga_snapshot_token");
  }
  if (lane.received_snapshot_token_id != request.snapshot.token_id ||
      lane.received_snapshot_generation != request.snapshot.snapshot_generation) {
    return Refuse(request, "parallel_pipeline_snapshot_token_mismatch",
                  "executor.parallel_pipeline.snapshot_token_mismatch",
                  "worker_snapshot_token_mismatched_shared_snapshot");
  }
  if (WorkerClaimsForbiddenAuthority(lane)) {
    return Refuse(request, "parallel_pipeline_worker_forbidden_authority",
                  "executor.parallel_pipeline.worker_forbidden_authority",
                  "worker_attempted_to_own_finality_visibility_or_recovery");
  }
  if (lane.cancel_requested) {
    return Refuse(request, "parallel_pipeline_cancelled",
                  "executor.parallel_pipeline.cancelled",
                  "cancelled_during_worker_lane");
  }
  if (lane.pressure_exceeded ||
      (request.quotas.max_worker_pressure_bytes != 0 &&
       lane.byte_count > request.quotas.max_worker_pressure_bytes)) {
    return Refuse(request, "parallel_pipeline_worker_pressure",
                  "executor.parallel_pipeline.worker_pressure",
                  "worker_pressure_or_backpressure_threshold_exceeded");
  }
  ParallelPhysicalPipelineResult ok;
  ok.status = OkStatus();
  return ok;
}

ParallelPhysicalPipelineResult ValidateWorkerResult(
    const ParallelPhysicalPipelineRequest& request,
    const ParallelPhysicalWorkerLane& lane,
    const ParallelPhysicalWorkerExecutionResult& worker_result) {
  if (!worker_result.ok()) {
    ParallelPhysicalPipelineResult result;
    result.status =
        worker_result.status.ok() ? RefusalStatus() : worker_result.status;
    result.fail_closed = true;
    result.diagnostic = worker_result.diagnostic.diagnostic_code.empty()
                            ? MakeParallelPhysicalPipelineDiagnostic(
                                  result.status,
                                  "parallel_pipeline_worker_provider_failed",
                                  "executor.parallel_pipeline.worker_provider_failed",
                                  "worker_provider_returned_failure")
                            : worker_result.diagnostic;
    result.refusal_reasons.push_back("worker_provider_returned_failure");
    result.evidence = BaseEvidence(request.family, request.snapshot);
    AppendEvidence(&result.evidence, worker_result.evidence);
    result.evidence.push_back("parallel_pipeline.fail_closed=true");
    return result;
  }
  if (worker_result.worker_id != lane.worker_id) {
    return Refuse(request, "parallel_pipeline_worker_result_mismatch",
                  "executor.parallel_pipeline.worker_result_mismatch",
                  "worker_result_id_mismatched_lane");
  }
  if (worker_result.snapshot_token_id != request.snapshot.token_id ||
      worker_result.snapshot_generation != request.snapshot.snapshot_generation) {
    return Refuse(request, "parallel_pipeline_snapshot_token_mismatch",
                  "executor.parallel_pipeline.snapshot_token_mismatch",
                  "worker_result_snapshot_mismatched_shared_snapshot");
  }
  if (worker_result.cancel_requested) {
    return Refuse(request, "parallel_pipeline_cancelled",
                  "executor.parallel_pipeline.cancelled",
                  "cancelled_during_worker_execution");
  }
  if (WorkerResultClaimsForbiddenAuthority(worker_result)) {
    return Refuse(request, "parallel_pipeline_worker_forbidden_authority",
                  "executor.parallel_pipeline.worker_forbidden_authority",
                  "worker_result_attempted_to_own_finality_visibility_or_recovery");
  }
  if (worker_result.final_rows_authorized) {
    return Refuse(request, "parallel_pipeline_worker_final_rows_refused",
                  "executor.parallel_pipeline.worker_final_rows_refused",
                  "worker_result_attempted_to_authorize_final_rows");
  }
  if (worker_result.worker_thread_id == std::thread::id{}) {
    return Refuse(request, "parallel_pipeline_worker_thread_required",
                  "executor.parallel_pipeline.worker_thread_required",
                  "worker_result_missing_thread_identity");
  }
  if (worker_result.worker_thread_id == std::this_thread::get_id()) {
    return Refuse(request, "parallel_pipeline_worker_not_parallel",
                  "executor.parallel_pipeline.worker_not_parallel",
                  "worker_result_reported_caller_thread_identity");
  }
  ParallelPhysicalPipelineResult ok;
  ok.status = OkStatus();
  return ok;
}

ParallelPhysicalPipelineResult MergeAndFinalize(
    const ParallelPhysicalPipelineRequest& request,
    ParallelPhysicalPipelineResult result,
    std::vector<CandidateSetRow> rows) {
  result.merged_candidates =
      MakeExactRowUuidOrderedCandidateSet(std::move(rows), request.authority,
                                          false);
  AppendEvidence(&result.evidence, result.merged_candidates.evidence);
  if (!result.merged_candidates.ok()) {
    result.status = result.merged_candidates.status;
    result.fail_closed = true;
    result.diagnostic = result.merged_candidates.diagnostic;
    result.refusal_reasons.push_back("odf090_candidate_set_merge_refused");
    result.evidence.push_back("parallel_pipeline.fail_closed=true");
    return result;
  }

  CandidateSet merged = result.merged_candidates.output;
  if (request.family == ParallelPhysicalPipelineFamily::kVectorExactScanRerank) {
    auto reranked = RerankCandidateSet(
        merged, [](const CandidateSetRow& row) { return row.score; },
        request.authority);
    AppendEvidence(&result.evidence, reranked.evidence);
    if (!reranked.ok()) {
      result.status = reranked.status;
      result.fail_closed = true;
      result.diagnostic = reranked.diagnostic;
      result.refusal_reasons.push_back("odf090_vector_rerank_refused");
      result.evidence.push_back("parallel_pipeline.fail_closed=true");
      return result;
    }
    merged = std::move(reranked.output);
  }

  result.finalized = FinalizeCandidateSetForExecutor(merged, request.authority);
  AppendEvidence(&result.evidence, result.finalized.evidence);
  if (!result.finalized.ok()) {
    result.status = result.finalized.status;
    result.fail_closed = true;
    result.diagnostic = result.finalized.recheck.diagnostic;
    result.refusal_reasons.push_back("odf090_exact_recheck_refused");
    result.evidence.push_back("parallel_pipeline.fail_closed=true");
    return result;
  }

  result.final_row_uuids = result.finalized.final_row_uuids;
  result.status = OkStatus();
  result.diagnostic = MakeParallelPhysicalPipelineDiagnostic(
      result.status, "parallel_pipeline_ok",
      "executor.parallel_pipeline.executed",
      "parallel physical pipeline finalized by merge phase");
  result.evidence.push_back("parallel_pipeline.final_rows_authorized_by_merge=true");
  result.evidence.push_back("parallel_pipeline.worker_rows_authorized=false");
  result.evidence.push_back("parallel_pipeline.deterministic_row_uuid_ordering=true");
  return result;
}

}  // namespace

const char* ParallelPhysicalPipelineFamilyName(
    ParallelPhysicalPipelineFamily family) {
  switch (family) {
    case ParallelPhysicalPipelineFamily::kPageScan:
      return "page_scan";
    case ParallelPhysicalPipelineFamily::kPageSummaryPrune:
      return "page_summary_prune";
    case ParallelPhysicalPipelineFamily::kIndexBuild:
      return "index_build";
    case ParallelPhysicalPipelineFamily::kSearchSegmentBuild:
      return "search_segment_build";
    case ParallelPhysicalPipelineFamily::kVectorExactScanRerank:
      return "vector_exact_scan_rerank";
    case ParallelPhysicalPipelineFamily::kTimeSeriesAggregate:
      return "time_series_aggregate";
    case ParallelPhysicalPipelineFamily::kGraphFrontierExpansion:
      return "graph_frontier_expansion";
    case ParallelPhysicalPipelineFamily::kCopyDecodeBindAppend:
      return "copy_decode_bind_append";
    case ParallelPhysicalPipelineFamily::kJoin:
      return "join";
    case ParallelPhysicalPipelineFamily::kDmlUpdate:
      return "dml_update";
    case ParallelPhysicalPipelineFamily::kUnsupported:
      break;
  }
  return "unsupported";
}

ParallelPhysicalPipelineResult ExecuteParallelPhysicalPipeline(
    const ParallelPhysicalPipelineRequest& request) {
  auto governance_request = request.resource_governance;
  governance_request.expected_family =
      agents::ResourceGovernanceFamily::kParallelPhysicalPipeline;
  const auto governance = agents::AdmitResourceGovernance(governance_request);
  if (governance.action == agents::ResourceGovernanceAction::kFailClosed) {
    return GovernanceRefuse(request, governance);
  }
  if (governance.action == agents::ResourceGovernanceAction::kSlowdownDegrade ||
      governance.action ==
          agents::ResourceGovernanceAction::kExactScalarFallback) {
    return GovernanceFallback(request, governance);
  }
  if (governance.action == agents::ResourceGovernanceAction::kCancel) {
    return GovernanceRefuse(request, governance);
  }
  if (!FamilySupported(request.family)) {
    return Refuse(request, "parallel_pipeline_unsupported_family",
                  "executor.parallel_pipeline.unsupported_family",
                  "unsupported_or_corrupt_parallel_pipeline_family");
  }
  if (request.cancellation.cancel_before_start) {
    return Refuse(request, "parallel_pipeline_cancelled",
                  "executor.parallel_pipeline.cancelled",
                  "cancelled_before_worker_admission");
  }
  auto snapshot_check = ValidateSnapshot(request);
  if (!snapshot_check.ok()) {
    return snapshot_check;
  }
  if (request.worker_lanes.empty()) {
    return Refuse(request, "parallel_pipeline_worker_required",
                  "executor.parallel_pipeline.worker_required",
                  "no_worker_lanes_submitted");
  }
  if (request.quotas.max_workers == 0 ||
      static_cast<u64>(request.worker_lanes.size()) >
          request.quotas.max_workers) {
    return Refuse(request, "parallel_pipeline_max_workers_exceeded",
                  "executor.parallel_pipeline.max_workers_exceeded",
                  "max_worker_lane_quota_exceeded");
  }

  ParallelPhysicalPipelineResult result;
  result.status = OkStatus();
  result.evidence = BaseEvidence(request.family, request.snapshot);
  result.evidence.push_back("parallel_pipeline.shared_snapshot_token.validated=true");
  AppendGovernanceEvidence(&result, governance);
  AppendRouteEvidence(request.family, &result.evidence);

  auto lanes = request.worker_lanes;
  std::stable_sort(lanes.begin(), lanes.end(),
                   [](const ParallelPhysicalWorkerLane& left,
                      const ParallelPhysicalWorkerLane& right) {
                     return left.worker_id < right.worker_id;
                   });

  std::set<u32> worker_ids;
  for (const auto& lane : lanes) {
    auto lane_check = ValidateLaneDescriptor(request, lane, &worker_ids);
    if (!lane_check.ok()) {
      return lane_check;
    }
  }

  const auto provider =
      request.worker_provider ? request.worker_provider
                              : ParallelPhysicalWorkerProvider(
                                    DefaultParallelPhysicalWorkerProvider);
  std::vector<ParallelPhysicalWorkerExecutionRequest> worker_requests;
  worker_requests.reserve(lanes.size());
  for (const auto& lane : lanes) {
    ParallelPhysicalWorkerExecutionRequest worker_request;
    worker_request.route_generation = request.route_generation;
    worker_request.family = request.family;
    worker_request.snapshot = request.snapshot;
    worker_request.quotas = request.quotas;
    worker_request.authority = request.authority;
    worker_request.lane = lane;
    worker_request.worker_route_descriptor = WorkerRouteDescriptor(
        request.family, lane.worker_id, request.snapshot.token_id,
        &result.evidence);
    worker_requests.push_back(std::move(worker_request));
  }

  std::vector<std::future<ParallelPhysicalWorkerExecutionResult>> futures;
  futures.reserve(worker_requests.size());
  for (const auto& worker_request : worker_requests) {
    futures.push_back(std::async(std::launch::async,
                                 [provider, worker_request]() mutable {
                                   try {
                                     return provider(worker_request);
                                   } catch (...) {
                                     ParallelPhysicalWorkerExecutionResult failed;
                                     failed.status = RefusalStatus();
                                     failed.fail_closed = true;
                                     failed.worker_id =
                                         worker_request.lane.worker_id;
                                     failed.snapshot_token_id =
                                         worker_request.snapshot.token_id;
                                     failed.snapshot_generation =
                                         worker_request.snapshot.snapshot_generation;
                                     failed.worker_thread_id =
                                         std::this_thread::get_id();
                                     failed.diagnostic =
                                         MakeParallelPhysicalPipelineDiagnostic(
                                             failed.status,
                                             "parallel_pipeline_worker_provider_threw",
                                             "executor.parallel_pipeline.worker_provider_threw",
                                             "worker_provider_threw_exception");
                                     failed.evidence.push_back(
                                         "parallel_worker.provider_exception=true");
                                     return failed;
                                   }
                                 }));
  }

  result.evidence.push_back("parallel_pipeline.actual_worker_threads=true");
  result.evidence.push_back("parallel_pipeline.worker_thread_count=" +
                            std::to_string(futures.size()));

  std::set<std::thread::id> distinct_thread_ids;
  std::vector<CandidateSetRow> candidate_rows;
  for (std::size_t i = 0; i < futures.size(); ++i) {
    auto worker_result = futures[i].get();
    const auto& lane = worker_requests[i].lane;
    auto worker_check = ValidateWorkerResult(request, lane, worker_result);
    if (!worker_check.ok()) {
      return worker_check;
    }
    const u64 result_fragments = ResultFragmentCount(worker_result);
    if (!AddWithinQuota(&result.counters.fragments_produced, result_fragments,
                        request.quotas.max_fragments)) {
      return Refuse(request, "parallel_pipeline_max_fragments_exceeded",
                    "executor.parallel_pipeline.max_fragments_exceeded",
                    "max_fragment_quota_exceeded");
    }
    if (!AddWithinQuota(
            &result.counters.candidate_rows_produced,
            static_cast<u64>(worker_result.candidate_rows.size()),
            request.quotas.max_candidate_rows)) {
      return Refuse(request, "parallel_pipeline_max_candidate_rows_exceeded",
                    "executor.parallel_pipeline.max_candidate_rows_exceeded",
                    "max_candidate_row_quota_exceeded");
    }
    if (!AddWithinQuota(&result.counters.bytes_produced,
                        worker_result.byte_count, request.quotas.max_bytes)) {
      return Refuse(request, "parallel_pipeline_max_bytes_exceeded",
                    "executor.parallel_pipeline.max_bytes_exceeded",
                    "max_byte_quota_exceeded");
    }

    AppendEvidence(&result.evidence, worker_result.evidence);
    result.evidence.push_back("parallel_pipeline.worker_thread_id=" +
                              ThreadIdString(worker_result.worker_thread_id));
    distinct_thread_ids.insert(worker_result.worker_thread_id);
    result.worker_thread_ids.push_back(worker_result.worker_thread_id);

    ParallelPhysicalPipelineFragment fragment;
    fragment.family = request.family;
    fragment.worker_id = worker_result.worker_id;
    fragment.fragment_ordinal = result.fragments.size();
    fragment.byte_count = worker_result.byte_count;
    fragment.candidate_rows = worker_result.candidate_rows;
    result.fragments.push_back(fragment);
    candidate_rows.insert(candidate_rows.end(),
                          worker_result.candidate_rows.begin(),
                          worker_result.candidate_rows.end());
    ++result.counters.workers_admitted;
    IncrementFamilyLane(request.family, &result.counters);
  }

  result.counters.actual_worker_threads =
      static_cast<u64>(distinct_thread_ids.size());
  result.evidence.push_back("parallel_pipeline.distinct_worker_thread_count=" +
                            std::to_string(distinct_thread_ids.size()));
  std::stable_sort(candidate_rows.begin(), candidate_rows.end(), RowUuidLess);
  return MergeAndFinalize(request, std::move(result),
                          std::move(candidate_rows));
}

DiagnosticRecord MakeParallelPhysicalPipelineDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail) {
  DiagnosticRecord diagnostic;
  diagnostic.status = status;
  diagnostic.diagnostic_code = std::move(diagnostic_code);
  diagnostic.message_key = std::move(message_key);
  if (!detail.empty()) {
    diagnostic.arguments.push_back({"detail", detail});
  }
  diagnostic.source_component = "engine.executor.parallel_physical_pipeline";
  diagnostic.remediation_hint = std::move(detail);
  return diagnostic;
}

}  // namespace scratchbird::engine::executor
