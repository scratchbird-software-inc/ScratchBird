// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-PARALLEL-PHYSICAL-PIPELINE-ANCHOR
#include "candidate_set.hpp"
#include "candidate_set_executor.hpp"
#include "resource_governance_admission.hpp"
#include "runtime_platform.hpp"

#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace scratchbird::engine::executor {

using scratchbird::core::platform::DiagnosticRecord;
using scratchbird::core::platform::Status;
using scratchbird::core::platform::u32;
using scratchbird::core::platform::u64;

enum class ParallelPhysicalPipelineFamily : u32 {
  kUnsupported = 0,
  kPageScan = 1,
  kPageSummaryPrune = 2,
  kIndexBuild = 3,
  kSearchSegmentBuild = 4,
  kVectorExactScanRerank = 5,
  kTimeSeriesAggregate = 6,
  kGraphFrontierExpansion = 7,
  kCopyDecodeBindAppend = 8,
  kJoin = 9,
  kDmlUpdate = 10
};

struct ParallelPhysicalSnapshotToken {
  std::string token_id;
  u64 snapshot_generation = 0;
  u64 transaction_number = 0;
  u64 visibility_high_water_mark = 0;
  u64 catalog_epoch = 0;
  u64 security_epoch = 0;
  u64 policy_epoch = 0;
  bool stale = false;
  bool engine_mga_snapshot = true;
  bool transaction_inventory_bound = true;
  bool catalog_security_policy_epochs_bound = true;
};

struct ParallelPhysicalPipelineQuotas {
  u64 max_workers = 4;
  u64 max_fragments = 1024;
  u64 max_candidate_rows = 100000;
  u64 max_bytes = 1024 * 1024;
  u64 max_worker_pressure_bytes = 0;
};

struct ParallelPhysicalPipelineCancellation {
  bool cancel_before_start = false;
};

struct ParallelPhysicalWorkerLane {
  u32 worker_id = 0;
  bool snapshot_token_present = true;
  std::string received_snapshot_token_id;
  u64 received_snapshot_generation = 0;
  bool received_snapshot_stale = false;
  u64 fragment_count = 0;
  u64 byte_count = 0;
  bool pressure_exceeded = false;
  bool cancel_requested = false;
  bool corrupt_family_descriptor = false;
  std::vector<scratchbird::core::index::CandidateSetRow> candidate_rows;

  bool worker_claims_transaction_finality = false;
  bool worker_claims_visibility_authority = false;
  bool worker_claims_security_authority = false;
  bool worker_claims_publication_authority = false;
  bool worker_claims_recovery_authority = false;
  bool parser_or_donor_finality_or_visibility_authority = false;
  bool write_after_stream_finality_or_recovery_authority = false;
  bool timestamp_finality_authority = false;
  bool uuid_ordering_finality_authority = false;
};

struct ParallelPhysicalWorkerExecutionRequest {
  u64 route_generation = 103;
  ParallelPhysicalPipelineFamily family =
      ParallelPhysicalPipelineFamily::kUnsupported;
  ParallelPhysicalSnapshotToken snapshot;
  ParallelPhysicalPipelineQuotas quotas;
  scratchbird::core::index::CandidateSetAuthorityContext authority;
  ParallelPhysicalWorkerLane lane;
  std::string worker_route_descriptor;
};

struct ParallelPhysicalWorkerExecutionResult {
  Status status;
  bool fail_closed = false;
  u32 worker_id = 0;
  std::string snapshot_token_id;
  u64 snapshot_generation = 0;
  u64 fragment_count = 0;
  u64 byte_count = 0;
  std::vector<scratchbird::core::index::CandidateSetRow> candidate_rows;
  std::vector<std::string> evidence;
  DiagnosticRecord diagnostic;
  std::thread::id worker_thread_id;
  bool cancel_requested = false;
  bool final_rows_authorized = false;
  bool worker_claims_transaction_finality = false;
  bool worker_claims_visibility_authority = false;
  bool worker_claims_security_authority = false;
  bool worker_claims_publication_authority = false;
  bool worker_claims_recovery_authority = false;
  bool parser_or_donor_finality_or_visibility_authority = false;
  bool write_after_stream_finality_or_recovery_authority = false;
  bool timestamp_finality_authority = false;
  bool uuid_ordering_finality_authority = false;

  bool ok() const { return status.ok() && !fail_closed; }
};

using ParallelPhysicalWorkerProvider =
    std::function<ParallelPhysicalWorkerExecutionResult(
        const ParallelPhysicalWorkerExecutionRequest&)>;

struct ParallelPhysicalPipelineRequest {
  u64 route_generation = 103;
  ParallelPhysicalPipelineFamily family =
      ParallelPhysicalPipelineFamily::kUnsupported;
  ParallelPhysicalSnapshotToken snapshot;
  ParallelPhysicalPipelineQuotas quotas;
  ParallelPhysicalPipelineCancellation cancellation;
  scratchbird::core::index::CandidateSetAuthorityContext authority;
  std::vector<ParallelPhysicalWorkerLane> worker_lanes;
  ParallelPhysicalWorkerProvider worker_provider;
  scratchbird::core::agents::ResourceGovernanceAdmissionRequest
      resource_governance;
};

struct ParallelPhysicalPipelineFragment {
  ParallelPhysicalPipelineFamily family =
      ParallelPhysicalPipelineFamily::kUnsupported;
  u32 worker_id = 0;
  u64 fragment_ordinal = 0;
  u64 byte_count = 0;
  std::vector<scratchbird::core::index::CandidateSetRow> candidate_rows;
};

struct ParallelPhysicalPipelineCounters {
  u64 workers_admitted = 0;
  u64 fragments_produced = 0;
  u64 candidate_rows_produced = 0;
  u64 bytes_produced = 0;
  u64 page_scan_lanes = 0;
  u64 page_summary_prune_lanes = 0;
  u64 index_build_lanes = 0;
  u64 search_segment_build_lanes = 0;
  u64 vector_exact_scan_rerank_lanes = 0;
  u64 time_series_aggregate_lanes = 0;
  u64 graph_frontier_expansion_lanes = 0;
  u64 copy_decode_bind_append_lanes = 0;
  u64 join_lanes = 0;
  u64 dml_update_lanes = 0;
  u64 actual_worker_threads = 0;
};

struct ParallelPhysicalPipelineResult {
  Status status;
  bool fail_closed = false;
  bool fallback_used = false;
  ParallelPhysicalPipelineCounters counters;
  std::vector<ParallelPhysicalPipelineFragment> fragments;
  scratchbird::core::index::CandidateSetResult merged_candidates;
  ExecutorCandidateSetFinalizeResult finalized;
  std::vector<scratchbird::core::platform::TypedUuid> final_row_uuids;
  std::vector<std::thread::id> worker_thread_ids;
  DiagnosticRecord diagnostic;
  std::vector<std::string> evidence;
  std::vector<std::string> refusal_reasons;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* ParallelPhysicalPipelineFamilyName(
    ParallelPhysicalPipelineFamily family);

ParallelPhysicalPipelineResult ExecuteParallelPhysicalPipeline(
    const ParallelPhysicalPipelineRequest& request);

DiagnosticRecord MakeParallelPhysicalPipelineDiagnostic(
    Status status,
    std::string diagnostic_code,
    std::string message_key,
    std::string detail = {});

}  // namespace scratchbird::engine::executor
