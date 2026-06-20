// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_operation_path_proof.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace api = scratchbird::engine::internal_api;

[[noreturn]] void Fail(std::string_view message) {
  std::cerr << "ipar_dml_operation_path_proof_gate: " << message << '\n';
  std::exit(EXIT_FAILURE);
}

void Require(bool condition, std::string_view message) {
  if (!condition) {
    Fail(message);
  }
}

bool HasEvidence(const std::vector<api::EngineEvidenceReference>& evidence,
                 std::string_view kind,
                 std::string_view id) {
  return std::any_of(evidence.begin(), evidence.end(), [&](const auto& item) {
    return item.evidence_kind == kind &&
           item.evidence_id.find(id) != std::string::npos;
  });
}

bool HasMetric(const std::vector<api::DmlOperationPathMetric>& metrics,
               std::string_view metric_id,
               std::string_view field) {
  return std::any_of(metrics.begin(), metrics.end(), [&](const auto& item) {
    return item.metric_id == metric_id && item.field == field &&
           !item.value.empty();
  });
}

api::DmlPipelineCpuDistributionSample CpuSample(api::EngineApiU64 seed) {
  api::DmlPipelineCpuDistributionSample sample;
  sample.parser_prepare_cpu_us = 10 + seed;
  sample.server_admission_cpu_us = 11 + seed;
  sample.authorization_cpu_us = 12 + seed;
  sample.target_lookup_cpu_us = 13 + seed;
  sample.row_encode_cpu_us = 14 + seed;
  sample.enqueue_cpu_us = 15 + seed;
  sample.writeback_cpu_us = 16 + seed;
  sample.index_maintenance_cpu_us = 17 + seed;
  sample.commit_fence_cpu_us = 18 + seed;
  sample.cleanup_cpu_us = 19 + seed;
  sample.diagnostics_cpu_us = 20 + seed;
  sample.distinct_worker_threads = 4;
  sample.queue_wait_us = 2;
  sample.lock_wait_us = 3;
  sample.syscall_wait_us = 4;
  sample.foreground_saturation_point_recorded = true;
  return sample;
}

api::DmlTargetAccessPlan AcceptedAccessPlan(api::DmlTargetAccessKind kind) {
  api::DmlTargetAccessPlan plan;
  plan.ok = true;
  plan.access_kind = kind;
  plan.physical_access_kind = "scalar_btree_lookup";
  plan.executor_capability = "executor.index_point_lookup";
  plan.relation_uuid = "018f0000-0000-7000-8000-000000000001";
  plan.index_uuid = "018f0000-0000-7000-8000-000000000002";
  plan.estimated_rows = kind == api::DmlTargetAccessKind::unique_index_lookup ? 1 : 8;
  plan.evidence.push_back("mga_finality_authority=engine_transaction_inventory");
  return plan;
}

api::DmlRowLocatorStreamResult AcceptedLocator(api::DmlRowLocatorStreamSource source) {
  api::DmlRowLocatorStreamResult result;
  result.ok = true;
  result.source = source;
  result.runtime_route_capability = true;
  result.benchmark_clean = true;
  result.locators.push_back({"018f0000-0000-7000-8000-000000000010",
                             "018f0000-0000-7000-8000-000000000011",
                             "018f0000-0000-7000-8000-000000000002",
                             1,
                             0,
                             true,
                             true,
                             true});
  result.evidence.push_back({"dml_row_locator_stream_no_table_scan",
                             "physical_locator_stream_consumed"});
  return result;
}

api::DmlIndexWritePathResult AcceptedIndexWrite(api::EngineApiU64 inserts,
                                                api::EngineApiU64 deletes,
                                                api::EngineApiU64 noops,
                                                api::EngineApiU64 merge_events) {
  api::DmlIndexWritePathResult result;
  result.ok = true;
  result.physical_inserts = inserts;
  result.physical_deletes = deletes;
  result.unchanged_key_noops = noops;
  result.merge_events = merge_events;
  result.evidence.push_back({"dml_index_write_path",
                             "physical_and_delta_route_accepted"});
  return result;
}

api::DmlOperationPathProofRequest Base(api::DmlOperationPathFamily family,
                                       api::EngineApiU64 seed) {
  api::DmlOperationPathProofRequest request;
  request.family = family;
  request.proof_run_id = "ipar-r0-operation-path-proof-" + std::to_string(seed);
  request.mode = "warm_prepared_batch";
  request.page_size = 8192;
  request.rows_per_second = 100000 + seed;
  request.setup_us = 90 + seed;
  request.write_ticket_wait_us = 5;
  request.commit_wait_us = 7;
  request.rollback_verify_us = 8;
  request.recovery_verify_us = 9;
  request.cache_hit_count = 6;
  request.server_uuid_validation_count = 6;
  request.transaction_fence_batch_count = 2;
  request.cpu_distribution = CpuSample(seed);
  request.mixed_workload_matrix = true;
  request.execution_control_links = true;
  request.dependency_order_enforced = true;
  request.budget_baseline_present = true;
  request.stop_gate_enforced = true;
  request.wave_ownership_enforced = true;
  request.target_access_plan =
      AcceptedAccessPlan(api::DmlTargetAccessKind::unique_index_lookup);
  request.locator_stream =
      AcceptedLocator(api::DmlRowLocatorStreamSource::physical_unique_btree_point);
  request.index_write_path = AcceptedIndexWrite(1, 0, 0, 0);
  return request;
}

std::vector<api::DmlOperationPathProofRequest> BuildRequests() {
  std::vector<api::DmlOperationPathProofRequest> requests;

  auto insert = Base(api::DmlOperationPathFamily::insert_rows, 24);
  insert.input_rows = 64;
  insert.row_encoder_cache = true;
  insert.append_cursor_initialized_pages = true;
  insert.sequence_defaults_prefetched = true;
  insert.constraint_prefetch = true;
  insert.index_route_planned = true;
  insert.async_write_tickets = true;
  insert.post_commit_agents = true;
  insert.canonical_bulk_writer = true;
  insert.index_write_path = AcceptedIndexWrite(64, 0, 0, 0);
  requests.push_back(insert);

  auto update = Base(api::DmlOperationPathFamily::update_rows, 25);
  update.affected_rows = 16;
  update.candidate_lookup = true;
  update.old_row_decode_avoided = true;
  update.changed_column_mask = true;
  update.row_overlay_reuse = true;
  update.no_op_update_detection = true;
  update.mga_replacement_versions = true;
  update.old_reader_visibility_preserved = true;
  update.index_write_path = AcceptedIndexWrite(16, 16, 4, 0);
  requests.push_back(update);

  auto delete_rows = Base(api::DmlOperationPathFamily::delete_rows, 26);
  delete_rows.affected_rows = 12;
  delete_rows.candidate_lookup = true;
  delete_rows.tombstone_marker = true;
  delete_rows.index_delete_delta = true;
  delete_rows.cleanup_debt_ledger = true;
  delete_rows.horizon_cleanup_debt_owned = true;
  delete_rows.old_reader_visibility_preserved = true;
  delete_rows.index_write_path = AcceptedIndexWrite(0, 12, 0, 0);
  requests.push_back(delete_rows);

  auto merge = Base(api::DmlOperationPathFamily::merge_upsert_rows, 27);
  merge.source_rows = 32;
  merge.affected_rows = 20;
  merge.keyed_probe = true;
  merge.bucketized_route = true;
  merge.routed_insert_fast_path = true;
  merge.routed_update_fast_path = true;
  merge.exact_conflict_proof = true;
  merge.index_write_path = AcceptedIndexWrite(12, 8, 0, 32);
  requests.push_back(merge);

  auto insert_select = Base(api::DmlOperationPathFamily::insert_select_rows, 28);
  insert_select.source_rows = 48;
  insert_select.source_snapshot_bound = true;
  insert_select.source_iterator_streaming = true;
  insert_select.vector_validation = true;
  insert_select.canonical_bulk_writer = true;
  insert_select.materializes_only_when_required = true;
  insert_select.index_write_path = AcceptedIndexWrite(48, 0, 0, 0);
  requests.push_back(insert_select);

  auto copy = Base(api::DmlOperationPathFamily::copy_rows, 29);
  copy.input_rows = 80;
  copy.copy_decoder = true;
  copy.copy_decoder_route_local = true;
  copy.canonical_bulk_writer = true;
  copy.batching = true;
  copy.async_write_tickets = true;
  copy.row_diagnostics = true;
  copy.index_write_path = AcceptedIndexWrite(80, 0, 0, 0);
  requests.push_back(copy);

  return requests;
}

void TestOperationRows() {
  for (const auto& request : BuildRequests()) {
    const auto result = api::BuildDmlOperationPathProof(request);
    if (!result.ok) {
      std::cerr << result.diagnostic.code << ':' << result.diagnostic.detail << '\n';
    }
    Require(result.ok, "operation proof should pass");
    Require(HasEvidence(result.evidence, "ipar_tracker_row", result.tracker_id),
            "operation tracker evidence missing");
    Require(HasEvidence(result.evidence, "ipar_acceptance_gate", result.gate_id),
            "operation gate evidence missing");
    Require(HasMetric(result.metrics, result.metric_id, "rows_per_second"),
            "operation throughput metric missing");
    Require(HasMetric(result.metrics, "IPAR-M007", "parser_prepare_cpu_us"),
            "CPU distribution inventory metric missing");
    Require(HasEvidence(result.evidence,
                        "operation_authority",
                        "engine_sblr_uuid_only_durable_mga_inventory_finality"),
            "MGA/SBLR authority proof missing");
  }
}

void TestMatrixRows() {
  const auto matrix = api::BuildDmlOperationPathProofMatrix(BuildRequests());
  if (!matrix.ok) {
    std::cerr << matrix.diagnostic.code << ':' << matrix.diagnostic.detail << '\n';
  }
  Require(matrix.ok, "operation path matrix should pass");
  Require(HasEvidence(matrix.evidence, "ipar_tracker_row", "IPAR-P0-07"),
          "P0 CPU inventory row evidence missing");
  Require(HasEvidence(matrix.evidence, "ipar_tracker_row", "IPAR-P7-14"),
          "around-DML proof row evidence missing");
  Require(HasEvidence(matrix.evidence, "ipar_tracker_row", "IPAR-P7-20"),
          "end-to-end proof row evidence missing");
  Require(HasEvidence(matrix.evidence, "ipar_tracker_row", "IPAR-P7-21"),
          "mixed workload proof row evidence missing");
  Require(HasEvidence(matrix.evidence, "ipar_tracker_row", "IPAR-P7-22"),
          "execution control proof row evidence missing");
  Require(HasMetric(matrix.metrics, "IPAR-M131", "operation_count"),
          "end-to-end operation count metric missing");
  Require(HasMetric(matrix.metrics, "IPAR-M138", "mixed_workload_matrix"),
          "mixed workload matrix metric missing");
  Require(HasMetric(matrix.metrics, "IPAR-M140", "stop_gate_enforced"),
          "execution control metric missing");
}

void TestAuthorityRefusal() {
  auto requests = BuildRequests();
  requests.front().parser_sql_execution = true;
  const auto result = api::BuildDmlOperationPathProof(requests.front());
  Require(!result.ok, "parser SQL execution authority drift was accepted");
  Require(result.diagnostic.detail.find("authority_boundary_incomplete") !=
              std::string::npos,
          "authority drift diagnostic mismatch");
}

}  // namespace

int main() {
  TestOperationRows();
  TestMatrixRows();
  TestAuthorityRefusal();
  return EXIT_SUCCESS;
}
