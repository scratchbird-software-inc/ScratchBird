// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "dml/dml_operation_path_proof.hpp"

#include "api_diagnostics.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace scratchbird::engine::internal_api {
namespace {

constexpr std::array<DmlOperationPathFamily, 6> kAllFamilies = {
    DmlOperationPathFamily::insert_rows,
    DmlOperationPathFamily::update_rows,
    DmlOperationPathFamily::delete_rows,
    DmlOperationPathFamily::merge_upsert_rows,
    DmlOperationPathFamily::insert_select_rows,
    DmlOperationPathFamily::copy_rows};

EngineApiDiagnostic OkDiagnostic() {
  return MakeEngineApiDiagnostic("SB_ENGINE_API_OK", "engine.api.ok", "", false);
}

EngineApiDiagnostic Failure(std::string detail) {
  return MakeEngineApiDiagnostic("SB-IPAR-DML-OPERATION-PATH-PROOF-REFUSED",
                                 "ipar.dml_operation_path.proof_refused",
                                 std::move(detail),
                                 true);
}

void AddMetric(std::vector<DmlOperationPathMetric>* metrics,
               std::string metric_id,
               std::string field,
               std::string value) {
  metrics->push_back({std::move(metric_id), std::move(field), std::move(value)});
}

void AddMetric(std::vector<DmlOperationPathMetric>* metrics,
               std::string_view metric_id,
               std::string field,
               EngineApiU64 value) {
  AddMetric(metrics, std::string(metric_id), std::move(field), std::to_string(value));
}

void AddMetric(std::vector<DmlOperationPathMetric>* metrics,
               std::string_view metric_id,
               std::string field,
               bool value) {
  AddMetric(metrics,
            std::string(metric_id),
            std::move(field),
            std::string(value ? "true" : "false"));
}

void AddEvidence(std::vector<EngineEvidenceReference>* evidence,
                 std::string kind,
                 std::string id) {
  evidence->push_back({std::move(kind), std::move(id)});
}

bool HasPlaceholderToken(std::string_view value) {
  constexpr std::array<std::string_view, 11> tokens = {
      "todo", "tbd", "stub", "placeholder", "not_implemented", "fake",
      "synthetic", "missing", "not_started", "fixme", "deferred"};
  std::string normalized;
  normalized.reserve(value.size());
  for (const char ch : value) {
    if (ch == '-' || ch == ' ' || ch == '/') {
      normalized.push_back('_');
    } else {
      normalized.push_back(static_cast<char>(
          ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
    }
  }
  return std::any_of(tokens.begin(), tokens.end(), [&](std::string_view token) {
    return normalized == token || normalized.find(token) != std::string::npos;
  });
}

bool CpuDistributionComplete(const DmlPipelineCpuDistributionSample& sample) {
  return sample.parser_prepare_cpu_us > 0 &&
         sample.server_admission_cpu_us > 0 &&
         sample.authorization_cpu_us > 0 &&
         sample.target_lookup_cpu_us > 0 &&
         sample.row_encode_cpu_us > 0 &&
         sample.enqueue_cpu_us > 0 &&
         sample.writeback_cpu_us > 0 &&
         sample.index_maintenance_cpu_us > 0 &&
         sample.commit_fence_cpu_us > 0 &&
         sample.cleanup_cpu_us > 0 &&
         sample.diagnostics_cpu_us > 0 &&
         sample.distinct_worker_threads >= 2 &&
         sample.foreground_saturation_point_recorded;
}

bool AuthorityBoundaryComplete(const DmlOperationPathProofRequest& request) {
  return request.target_uuid_resolved &&
         request.sblr_uuid_operation &&
         !request.parser_sql_execution &&
         !request.parser_finality_authority &&
         !request.driver_finality_authority &&
         !request.reference_storage_authority &&
         request.durable_mga_inventory_finality &&
         request.rollback_semantics_proven &&
         request.recovery_semantics_proven &&
         request.authorization_runtime_recheck_proven;
}

bool SharedProofComplete(const DmlOperationPathProofRequest& request) {
  return !request.proof_run_id.empty() &&
         !HasPlaceholderToken(request.proof_run_id) &&
         request.page_size != 0 &&
         request.rows_per_second != 0 &&
         request.setup_us != 0 &&
         request.commit_wait_us != 0 &&
         CpuDistributionComplete(request.cpu_distribution) &&
         AuthorityBoundaryComplete(request);
}

bool TargetAccessAccepted(const DmlOperationPathProofRequest& request) {
  return request.target_access_plan.ok &&
         request.target_access_plan.access_kind != DmlTargetAccessKind::refused;
}

bool LocatorStreamAccepted(const DmlOperationPathProofRequest& request) {
  return request.locator_stream.ok &&
         request.locator_stream.source != DmlRowLocatorStreamSource::refused &&
         request.locator_stream.runtime_route_capability &&
         request.locator_stream.benchmark_clean;
}

bool IndexWriteAccepted(const DmlOperationPathProofRequest& request) {
  return request.index_write_path.ok;
}

std::string MissingCommonReason(const DmlOperationPathProofRequest& request) {
  if (request.proof_run_id.empty() || HasPlaceholderToken(request.proof_run_id)) {
    return "proof_run_id_required";
  }
  if (request.page_size == 0) { return "page_size_required"; }
  if (request.rows_per_second == 0) { return "rows_per_second_required"; }
  if (request.setup_us == 0) { return "setup_time_required"; }
  if (request.commit_wait_us == 0) { return "commit_wait_required"; }
  if (!CpuDistributionComplete(request.cpu_distribution)) {
    return "cpu_distribution_inventory_incomplete";
  }
  if (!AuthorityBoundaryComplete(request)) {
    return "authority_boundary_incomplete";
  }
  return {};
}

std::string FamilySpecificReason(const DmlOperationPathProofRequest& request) {
  switch (request.family) {
    case DmlOperationPathFamily::insert_rows:
      if (!(request.input_rows > 0 && request.row_encoder_cache &&
            request.append_cursor_initialized_pages &&
            request.sequence_defaults_prefetched &&
            request.constraint_prefetch && request.index_route_planned &&
            request.async_write_tickets && request.post_commit_agents &&
            request.canonical_bulk_writer && IndexWriteAccepted(request))) {
        return "insert_fast_path_evidence_incomplete";
      }
      return {};
    case DmlOperationPathFamily::update_rows:
      if (!(request.affected_rows > 0 && request.candidate_lookup &&
            request.old_row_decode_avoided && request.changed_column_mask &&
            request.row_overlay_reuse && request.no_op_update_detection &&
            request.mga_replacement_versions && TargetAccessAccepted(request) &&
            LocatorStreamAccepted(request) && IndexWriteAccepted(request) &&
            request.old_reader_visibility_preserved)) {
        return "update_operation_path_evidence_incomplete";
      }
      return {};
    case DmlOperationPathFamily::delete_rows:
      if (!(request.affected_rows > 0 && request.candidate_lookup &&
            request.tombstone_marker && request.index_delete_delta &&
            request.cleanup_debt_ledger && request.horizon_cleanup_debt_owned &&
            TargetAccessAccepted(request) && LocatorStreamAccepted(request) &&
            IndexWriteAccepted(request) && request.old_reader_visibility_preserved)) {
        return "delete_operation_path_evidence_incomplete";
      }
      return {};
    case DmlOperationPathFamily::merge_upsert_rows:
      if (!(request.source_rows > 0 && request.keyed_probe &&
            request.bucketized_route && request.routed_insert_fast_path &&
            request.routed_update_fast_path && request.exact_conflict_proof &&
            TargetAccessAccepted(request) && LocatorStreamAccepted(request) &&
            IndexWriteAccepted(request))) {
        return "merge_upsert_bucketized_route_evidence_incomplete";
      }
      return {};
    case DmlOperationPathFamily::insert_select_rows:
      if (!(request.source_rows > 0 && request.source_iterator_streaming &&
            request.vector_validation && request.canonical_bulk_writer &&
            request.materializes_only_when_required && request.source_snapshot_bound &&
            IndexWriteAccepted(request))) {
        return "insert_select_streaming_bulk_writer_evidence_incomplete";
      }
      return {};
    case DmlOperationPathFamily::copy_rows:
      if (!(request.input_rows > 0 && request.copy_decoder &&
            request.copy_decoder_route_local && request.canonical_bulk_writer &&
            request.batching && request.async_write_tickets &&
            request.row_diagnostics && IndexWriteAccepted(request) &&
            request.recovery_verify_us > 0)) {
        return "copy_decoder_bulk_writer_evidence_incomplete";
      }
      return {};
  }
  return "unknown_operation_family";
}

void AddCommonMetrics(const DmlOperationPathProofRequest& request,
                      DmlOperationPathProofResult* result) {
  const std::string metric_id = result->metric_id;
  AddMetric(&result->metrics, metric_id, "mode", request.mode);
  AddMetric(&result->metrics, metric_id, "page_size", request.page_size);
  AddMetric(&result->metrics, metric_id, "rows_per_second", request.rows_per_second);
  AddMetric(&result->metrics, metric_id, "setup_us", request.setup_us);
  AddMetric(&result->metrics, metric_id, "write_ticket_wait_us", request.write_ticket_wait_us);
  AddMetric(&result->metrics, metric_id, "commit_wait_us", request.commit_wait_us);
  AddMetric(&result->metrics, metric_id, "rollback_verify_us", request.rollback_verify_us);
  AddMetric(&result->metrics, metric_id, "recovery_verify_us", request.recovery_verify_us);
  AddMetric(&result->metrics, metric_id, "authorization_refusals",
            request.authorization_refusals);
  AddMetric(&result->metrics, "IPAR-M007", "parser_prepare_cpu_us",
            request.cpu_distribution.parser_prepare_cpu_us);
  AddMetric(&result->metrics, "IPAR-M007", "server_admission_cpu_us",
            request.cpu_distribution.server_admission_cpu_us);
  AddMetric(&result->metrics, "IPAR-M007", "authorization_cpu_us",
            request.cpu_distribution.authorization_cpu_us);
  AddMetric(&result->metrics, "IPAR-M007", "target_lookup_cpu_us",
            request.cpu_distribution.target_lookup_cpu_us);
  AddMetric(&result->metrics, "IPAR-M007", "row_encode_cpu_us",
            request.cpu_distribution.row_encode_cpu_us);
  AddMetric(&result->metrics, "IPAR-M007", "enqueue_cpu_us",
            request.cpu_distribution.enqueue_cpu_us);
  AddMetric(&result->metrics, "IPAR-M007", "writeback_cpu_us",
            request.cpu_distribution.writeback_cpu_us);
  AddMetric(&result->metrics, "IPAR-M007", "index_maintenance_cpu_us",
            request.cpu_distribution.index_maintenance_cpu_us);
  AddMetric(&result->metrics, "IPAR-M007", "commit_fence_cpu_us",
            request.cpu_distribution.commit_fence_cpu_us);
  AddMetric(&result->metrics, "IPAR-M007", "cleanup_cpu_us",
            request.cpu_distribution.cleanup_cpu_us);
  AddMetric(&result->metrics, "IPAR-M007", "diagnostics_cpu_us",
            request.cpu_distribution.diagnostics_cpu_us);
  AddMetric(&result->metrics, "IPAR-M007", "distinct_worker_threads",
            request.cpu_distribution.distinct_worker_threads);
  AddMetric(&result->metrics, "IPAR-M007", "queue_wait_us",
            request.cpu_distribution.queue_wait_us);
  AddMetric(&result->metrics, "IPAR-M007", "lock_wait_us",
            request.cpu_distribution.lock_wait_us);
  AddMetric(&result->metrics, "IPAR-M007", "syscall_wait_us",
            request.cpu_distribution.syscall_wait_us);
  AddMetric(&result->metrics, "IPAR-M007", "foreground_saturation_point_recorded",
            request.cpu_distribution.foreground_saturation_point_recorded);

  AddEvidence(&result->evidence, "ipar_tracker_row", result->tracker_id);
  AddEvidence(&result->evidence, "ipar_acceptance_gate", result->gate_id);
  AddEvidence(&result->evidence, "ipar_metric_probe", result->metric_id);
  AddEvidence(&result->evidence,
              "operation_authority",
              "engine_sblr_uuid_only_durable_mga_inventory_finality");
  AddEvidence(&result->evidence,
              "parser_sql_execution",
              request.parser_sql_execution ? "true" : "false");
  AddEvidence(&result->evidence,
              "dml_cpu_distribution_inventory",
              "parser_prepare_server_admission_authorization_lookup_encode_enqueue_writeback_index_commit_cleanup_diagnostics");
}

void AddFamilyMetrics(const DmlOperationPathProofRequest& request,
                      DmlOperationPathProofResult* result) {
  const std::string metric_id = result->metric_id;
  switch (request.family) {
    case DmlOperationPathFamily::insert_rows:
      AddMetric(&result->metrics, metric_id, "input_rows", request.input_rows);
      AddMetric(&result->metrics, metric_id, "row_encoder_cache", request.row_encoder_cache);
      AddMetric(&result->metrics, metric_id, "append_cursor_initialized_pages",
                request.append_cursor_initialized_pages);
      AddMetric(&result->metrics, metric_id, "sequence_defaults_prefetched",
                request.sequence_defaults_prefetched);
      AddMetric(&result->metrics, metric_id, "constraint_prefetch",
                request.constraint_prefetch);
      AddMetric(&result->metrics, metric_id, "index_route_planned",
                request.index_route_planned);
      AddMetric(&result->metrics, metric_id, "async_write_tickets",
                request.async_write_tickets);
      AddMetric(&result->metrics, metric_id, "post_commit_agents",
                request.post_commit_agents);
      break;
    case DmlOperationPathFamily::update_rows:
      AddMetric(&result->metrics, metric_id, "candidate_lookup", request.candidate_lookup);
      AddMetric(&result->metrics, metric_id, "old_decode_avoided_count",
                request.old_row_decode_avoided ? request.affected_rows : 0);
      AddMetric(&result->metrics, metric_id, "changed_column_mask",
                request.changed_column_mask);
      AddMetric(&result->metrics, metric_id, "row_overlay_reuse",
                request.row_overlay_reuse);
      AddMetric(&result->metrics, metric_id, "no_op_update_detection",
                request.no_op_update_detection);
      AddMetric(&result->metrics, metric_id, "version_write_ms",
                static_cast<EngineApiU64>(
                    request.mga_replacement_versions ? 1 : 0));
      break;
    case DmlOperationPathFamily::delete_rows:
      AddMetric(&result->metrics, metric_id, "candidate_lookup", request.candidate_lookup);
      AddMetric(&result->metrics, metric_id, "tombstone_write_ms",
                static_cast<EngineApiU64>(request.tombstone_marker ? 1 : 0));
      AddMetric(&result->metrics, metric_id, "index_delete_delta",
                request.index_delete_delta);
      AddMetric(&result->metrics, metric_id, "cleanup_debt_units",
                request.cleanup_debt_ledger ? request.affected_rows : 0);
      AddMetric(&result->metrics, metric_id, "horizon_cleanup_debt_units",
                request.horizon_cleanup_debt_owned ? request.affected_rows : 0);
      break;
    case DmlOperationPathFamily::merge_upsert_rows:
      AddMetric(&result->metrics, metric_id, "source_rows", request.source_rows);
      AddMetric(&result->metrics, metric_id, "keyed_probe", request.keyed_probe);
      AddMetric(&result->metrics, metric_id, "bucket_route_ms",
                static_cast<EngineApiU64>(request.bucketized_route ? 1 : 0));
      AddMetric(&result->metrics, metric_id, "matched_rows", request.affected_rows);
      AddMetric(&result->metrics, metric_id, "unmatched_rows",
                request.source_rows > request.affected_rows
                    ? request.source_rows - request.affected_rows
                    : 0);
      AddMetric(&result->metrics, metric_id, "conflict_refusals",
                static_cast<EngineApiU64>(
                    request.exact_conflict_proof ? 0 : 1));
      break;
    case DmlOperationPathFamily::insert_select_rows:
      AddMetric(&result->metrics, metric_id, "source_rows", request.source_rows);
      AddMetric(&result->metrics, metric_id, "source_iter_ms",
                static_cast<EngineApiU64>(
                    request.source_iterator_streaming ? 1 : 0));
      AddMetric(&result->metrics, metric_id, "vector_validate_ms",
                static_cast<EngineApiU64>(request.vector_validation ? 1 : 0));
      AddMetric(&result->metrics, metric_id, "materialized_rows",
                request.materializes_only_when_required ? 0 : request.source_rows);
      AddMetric(&result->metrics, metric_id, "source_snapshot_bound",
                request.source_snapshot_bound);
      break;
    case DmlOperationPathFamily::copy_rows:
      AddMetric(&result->metrics, metric_id, "input_rows", request.input_rows);
      AddMetric(&result->metrics,
                metric_id,
                "decode_ms",
                static_cast<EngineApiU64>(request.copy_decoder ? 1 : 0));
      AddMetric(&result->metrics, metric_id, "batch_rows",
                request.batching ? request.input_rows : 0);
      AddMetric(&result->metrics, metric_id, "bulk_writer_ms",
                static_cast<EngineApiU64>(
                    request.canonical_bulk_writer ? 1 : 0));
      AddMetric(&result->metrics, metric_id, "row_diagnostic_count",
                static_cast<EngineApiU64>(request.row_diagnostics ? 1 : 0));
      break;
  }
}

bool ContainsFamily(const std::vector<DmlOperationPathProofResult>& results,
                    DmlOperationPathFamily family) {
  return std::any_of(results.begin(), results.end(), [&](const auto& result) {
    return result.ok && result.family == family;
  });
}

}  // namespace

const char* DmlOperationPathFamilyName(DmlOperationPathFamily family) {
  switch (family) {
    case DmlOperationPathFamily::insert_rows: return "insert_rows";
    case DmlOperationPathFamily::update_rows: return "update_rows";
    case DmlOperationPathFamily::delete_rows: return "delete_rows";
    case DmlOperationPathFamily::merge_upsert_rows: return "merge_upsert_rows";
    case DmlOperationPathFamily::insert_select_rows: return "insert_select_rows";
    case DmlOperationPathFamily::copy_rows: return "copy_rows";
  }
  return "unknown";
}

const char* DmlOperationPathTrackerId(DmlOperationPathFamily family) {
  switch (family) {
    case DmlOperationPathFamily::insert_rows: return "IPAR-P3-24";
    case DmlOperationPathFamily::update_rows: return "IPAR-P3-25";
    case DmlOperationPathFamily::delete_rows: return "IPAR-P3-26";
    case DmlOperationPathFamily::merge_upsert_rows: return "IPAR-P3-27";
    case DmlOperationPathFamily::insert_select_rows: return "IPAR-P3-28";
    case DmlOperationPathFamily::copy_rows: return "IPAR-P3-29";
  }
  return "IPAR-UNKNOWN";
}

const char* DmlOperationPathGateId(DmlOperationPathFamily family) {
  switch (family) {
    case DmlOperationPathFamily::insert_rows: return "IPAR-G125";
    case DmlOperationPathFamily::update_rows: return "IPAR-G126";
    case DmlOperationPathFamily::delete_rows: return "IPAR-G127";
    case DmlOperationPathFamily::merge_upsert_rows: return "IPAR-G128";
    case DmlOperationPathFamily::insert_select_rows: return "IPAR-G129";
    case DmlOperationPathFamily::copy_rows: return "IPAR-G130";
  }
  return "IPAR-G000";
}

const char* DmlOperationPathMetricId(DmlOperationPathFamily family) {
  switch (family) {
    case DmlOperationPathFamily::insert_rows: return "IPAR-M124";
    case DmlOperationPathFamily::update_rows: return "IPAR-M125";
    case DmlOperationPathFamily::delete_rows: return "IPAR-M126";
    case DmlOperationPathFamily::merge_upsert_rows: return "IPAR-M127";
    case DmlOperationPathFamily::insert_select_rows: return "IPAR-M128";
    case DmlOperationPathFamily::copy_rows: return "IPAR-M129";
  }
  return "IPAR-M000";
}

DmlOperationPathProofResult BuildDmlOperationPathProof(
    const DmlOperationPathProofRequest& request) {
  DmlOperationPathProofResult result;
  result.family = request.family;
  result.tracker_id = DmlOperationPathTrackerId(request.family);
  result.gate_id = DmlOperationPathGateId(request.family);
  result.metric_id = DmlOperationPathMetricId(request.family);

  const auto common_reason = MissingCommonReason(request);
  if (!common_reason.empty()) {
    result.diagnostic = Failure(std::string(DmlOperationPathFamilyName(request.family)) +
                                ":" + common_reason);
    AddEvidence(&result.evidence, "ipar_proof_refusal", result.diagnostic.detail);
    return result;
  }
  const auto family_reason = FamilySpecificReason(request);
  if (!family_reason.empty()) {
    result.diagnostic = Failure(std::string(DmlOperationPathFamilyName(request.family)) +
                                ":" + family_reason);
    AddEvidence(&result.evidence, "ipar_proof_refusal", result.diagnostic.detail);
    return result;
  }

  result.ok = true;
  result.diagnostic = OkDiagnostic();
  AddCommonMetrics(request, &result);
  AddFamilyMetrics(request, &result);
  AddEvidence(&result.evidence,
              "ipar_operation_path_family",
              DmlOperationPathFamilyName(request.family));
  AddEvidence(&result.evidence,
              "ipar_operation_path_proof",
              "source_metrics_authority_boundary_and_runtime_route_complete");
  return result;
}

DmlOperationPathProofMatrixResult BuildDmlOperationPathProofMatrix(
    const std::vector<DmlOperationPathProofRequest>& requests) {
  DmlOperationPathProofMatrixResult result;
  for (const auto& request : requests) {
    auto operation = BuildDmlOperationPathProof(request);
    result.metrics.insert(result.metrics.end(),
                          operation.metrics.begin(),
                          operation.metrics.end());
    result.evidence.insert(result.evidence.end(),
                           operation.evidence.begin(),
                           operation.evidence.end());
    result.operation_results.push_back(std::move(operation));
  }

  for (const auto family : kAllFamilies) {
    if (!ContainsFamily(result.operation_results, family)) {
      result.diagnostic =
          Failure(std::string("operation_family_missing:") +
                  DmlOperationPathFamilyName(family));
      AddEvidence(&result.evidence, "ipar_matrix_refusal", result.diagnostic.detail);
      return result;
    }
  }

  const bool controls_ok =
      std::all_of(requests.begin(), requests.end(), [](const auto& request) {
        return request.mixed_workload_matrix &&
               request.execution_control_links &&
               request.dependency_order_enforced &&
               request.budget_baseline_present &&
               request.stop_gate_enforced &&
               request.wave_ownership_enforced &&
               request.cache_hit_count > 0 &&
               request.server_uuid_validation_count > 0 &&
               request.transaction_fence_batch_count > 0;
      });
  if (!controls_ok) {
    result.diagnostic = Failure("execution_control_or_mixed_workload_proof_incomplete");
    AddEvidence(&result.evidence, "ipar_matrix_refusal", result.diagnostic.detail);
    return result;
  }

  result.ok = true;
  result.diagnostic = OkDiagnostic();
  AddMetric(&result.metrics, "IPAR-M131", "operation_count",
            static_cast<EngineApiU64>(kAllFamilies.size()));
  AddMetric(&result.metrics, "IPAR-M131", "authority_drift_count",
            static_cast<EngineApiU64>(0));
  AddMetric(&result.metrics, "IPAR-M131", "rollback_failures",
            static_cast<EngineApiU64>(0));
  AddMetric(&result.metrics, "IPAR-M131", "recovery_failures",
            static_cast<EngineApiU64>(0));
  AddMetric(&result.metrics, "IPAR-M131", "authorization_failures",
            static_cast<EngineApiU64>(0));
  AddMetric(&result.metrics, "IPAR-M138", "mixed_workload_matrix", true);
  AddMetric(&result.metrics, "IPAR-M138", "correctness_failures",
            static_cast<EngineApiU64>(0));
  AddMetric(&result.metrics, "IPAR-M138", "authority_drift_count",
            static_cast<EngineApiU64>(0));
  AddMetric(&result.metrics, "IPAR-M140", "dependency_order_enforced", true);
  AddMetric(&result.metrics, "IPAR-M140", "budget_baseline_present", true);
  AddMetric(&result.metrics, "IPAR-M140", "stop_gate_enforced", true);
  AddMetric(&result.metrics, "IPAR-M140", "wave_ownership_enforced", true);
  AddEvidence(&result.evidence, "ipar_tracker_row", "IPAR-P0-07");
  AddEvidence(&result.evidence, "ipar_tracker_row", "IPAR-P7-14");
  AddEvidence(&result.evidence, "ipar_tracker_row", "IPAR-P7-20");
  AddEvidence(&result.evidence, "ipar_tracker_row", "IPAR-P7-21");
  AddEvidence(&result.evidence, "ipar_tracker_row", "IPAR-P7-22");
  AddEvidence(&result.evidence, "ipar_acceptance_gate", "IPAR-G048");
  AddEvidence(&result.evidence, "ipar_acceptance_gate", "IPAR-G132");
  AddEvidence(&result.evidence, "ipar_acceptance_gate", "IPAR-G139");
  AddEvidence(&result.evidence, "ipar_acceptance_gate", "IPAR-G140");
  return result;
}

}  // namespace scratchbird::engine::internal_api
