// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

#include "api_types.hpp"
#include "dml/dml_index_write_path.hpp"
#include "dml/dml_row_locator_stream.hpp"
#include "dml/dml_target_access_plan.hpp"

#include <string>
#include <vector>

namespace scratchbird::engine::internal_api {

// SEARCH_KEY: SB_IPAR_DML_OPERATION_PATH_PROOF
enum class DmlOperationPathFamily {
  insert_rows,
  update_rows,
  delete_rows,
  merge_upsert_rows,
  insert_select_rows,
  copy_rows
};

struct DmlPipelineCpuDistributionSample {
  EngineApiU64 parser_prepare_cpu_us = 0;
  EngineApiU64 server_admission_cpu_us = 0;
  EngineApiU64 authorization_cpu_us = 0;
  EngineApiU64 target_lookup_cpu_us = 0;
  EngineApiU64 row_encode_cpu_us = 0;
  EngineApiU64 enqueue_cpu_us = 0;
  EngineApiU64 writeback_cpu_us = 0;
  EngineApiU64 index_maintenance_cpu_us = 0;
  EngineApiU64 commit_fence_cpu_us = 0;
  EngineApiU64 cleanup_cpu_us = 0;
  EngineApiU64 diagnostics_cpu_us = 0;
  EngineApiU64 distinct_worker_threads = 0;
  EngineApiU64 queue_wait_us = 0;
  EngineApiU64 lock_wait_us = 0;
  EngineApiU64 syscall_wait_us = 0;
  bool foreground_saturation_point_recorded = false;
};

struct DmlOperationPathProofRequest {
  DmlOperationPathFamily family = DmlOperationPathFamily::insert_rows;
  std::string proof_run_id;
  std::string mode = "warm_prepared_batch";
  EngineApiU64 page_size = 8192;
  EngineApiU64 source_rows = 0;
  EngineApiU64 input_rows = 0;
  EngineApiU64 affected_rows = 0;
  EngineApiU64 rows_per_second = 0;
  EngineApiU64 setup_us = 0;
  EngineApiU64 write_ticket_wait_us = 0;
  EngineApiU64 commit_wait_us = 0;
  EngineApiU64 rollback_verify_us = 0;
  EngineApiU64 recovery_verify_us = 0;
  EngineApiU64 authorization_refusals = 0;
  EngineApiU64 cache_hit_count = 0;
  EngineApiU64 parser_name_resolution_count = 0;
  EngineApiU64 driver_name_resolution_count = 0;
  EngineApiU64 server_uuid_validation_count = 0;
  EngineApiU64 transaction_fence_batch_count = 0;
  bool prepared = true;
  bool tls = false;
  bool non_tls_route_explicit = true;
  bool target_uuid_resolved = true;
  bool sblr_uuid_operation = true;
  bool parser_sql_execution = false;
  bool parser_finality_authority = false;
  bool driver_finality_authority = false;
  bool reference_storage_authority = false;
  bool durable_mga_inventory_finality = true;
  bool source_snapshot_bound = false;
  bool old_reader_visibility_preserved = false;
  bool rollback_semantics_proven = true;
  bool recovery_semantics_proven = true;
  bool authorization_runtime_recheck_proven = true;
  bool row_encoder_cache = false;
  bool append_cursor_initialized_pages = false;
  bool sequence_defaults_prefetched = false;
  bool constraint_prefetch = false;
  bool index_route_planned = false;
  bool async_write_tickets = false;
  bool post_commit_agents = false;
  bool candidate_lookup = false;
  bool old_row_decode_avoided = false;
  bool changed_column_mask = false;
  bool row_overlay_reuse = false;
  bool no_op_update_detection = false;
  bool mga_replacement_versions = false;
  bool tombstone_marker = false;
  bool index_delete_delta = false;
  bool cleanup_debt_ledger = false;
  bool horizon_cleanup_debt_owned = false;
  bool keyed_probe = false;
  bool bucketized_route = false;
  bool routed_insert_fast_path = false;
  bool routed_update_fast_path = false;
  bool exact_conflict_proof = false;
  bool source_iterator_streaming = false;
  bool vector_validation = false;
  bool canonical_bulk_writer = false;
  bool materializes_only_when_required = false;
  bool copy_decoder = false;
  bool copy_decoder_route_local = false;
  bool row_diagnostics = false;
  bool batching = false;
  bool mixed_workload_matrix = false;
  bool execution_control_links = false;
  bool dependency_order_enforced = false;
  bool budget_baseline_present = false;
  bool stop_gate_enforced = false;
  bool wave_ownership_enforced = false;
  DmlPipelineCpuDistributionSample cpu_distribution;
  DmlTargetAccessPlan target_access_plan;
  DmlRowLocatorStreamResult locator_stream;
  DmlIndexWritePathResult index_write_path;
};

struct DmlOperationPathMetric {
  std::string metric_id;
  std::string field;
  std::string value;
};

struct DmlOperationPathProofResult {
  bool ok = false;
  DmlOperationPathFamily family = DmlOperationPathFamily::insert_rows;
  std::string tracker_id;
  std::string gate_id;
  std::string metric_id;
  EngineApiDiagnostic diagnostic;
  std::vector<DmlOperationPathMetric> metrics;
  std::vector<EngineEvidenceReference> evidence;
};

struct DmlOperationPathProofMatrixResult {
  bool ok = false;
  EngineApiDiagnostic diagnostic;
  std::vector<DmlOperationPathProofResult> operation_results;
  std::vector<DmlOperationPathMetric> metrics;
  std::vector<EngineEvidenceReference> evidence;
};

const char* DmlOperationPathFamilyName(DmlOperationPathFamily family);
const char* DmlOperationPathTrackerId(DmlOperationPathFamily family);
const char* DmlOperationPathGateId(DmlOperationPathFamily family);
const char* DmlOperationPathMetricId(DmlOperationPathFamily family);

DmlOperationPathProofResult BuildDmlOperationPathProof(
    const DmlOperationPathProofRequest& request);

DmlOperationPathProofMatrixResult BuildDmlOperationPathProofMatrix(
    const std::vector<DmlOperationPathProofRequest>& requests);

}  // namespace scratchbird::engine::internal_api
