// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "sblr_hot_path_execution.hpp"

#include "sblr_execution_metrics.hpp"
#include "sblr_prepared_template.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace scratchbird::engine::sblr {
namespace exec = scratchbird::engine::executor;

namespace {

void AddCommonEvidence(const SblrHotPathExecutionRequest& request,
                       SblrHotPathExecutionResult* result) {
  result->evidence.push_back("sblr_hot_path.route_label=" +
                             request.route_label);
  result->evidence.push_back("sblr_hot_path.operation_id=" +
                             request.envelope.operation_id);
  result->evidence.push_back(
      "sblr_hot_path.execution_authority=engine_sblr_internal_envelope");
  result->evidence.push_back("sblr_hot_path.parser_execution_authority=false");
  result->evidence.push_back("sblr_hot_path.reference_execution_authority=false");
  result->evidence.push_back("sblr_hot_path.client_execution_authority=false");
  result->evidence.push_back(
      "sblr_hot_path.mga_finality_authority=engine_transaction_inventory");
  result->evidence.push_back("sblr_hot_path.visibility_authority=false");
  result->evidence.push_back("sblr_hot_path.recovery_authority=false");
  result->evidence.push_back("sblr_hot_path.security_authority=false");
  result->evidence.push_back(
      std::string("sblr_hot_path.engine_mga_snapshot_bound=") +
      (request.authority.engine_mga_snapshot_bound ? "true" : "false"));
  result->evidence.push_back(
      std::string("sblr_hot_path.transaction_inventory_proof_present=") +
      (request.authority.transaction_inventory_authoritative ? "true"
                                                             : "false"));
  result->evidence.push_back(
      std::string("sblr_hot_path.security_recheck_required=") +
      (request.authority.security_recheck_required ? "true" : "false"));
}

SblrHotPathExecutionResult Refuse(
    const SblrHotPathExecutionRequest& request,
    std::string code,
    std::string detail) {
  SblrHotPathExecutionResult result;
  result.ok = false;
  result.benchmark_clean = false;
  result.fail_closed = true;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  AddCommonEvidence(request, &result);
  result.evidence.push_back("sblr_hot_path.fail_closed=true");
  result.evidence.push_back("sblr_hot_path.refused=" + result.diagnostic_code);
  return result;
}

SblrHotPathExecutionResult Fallback(
    const SblrHotPathExecutionRequest& request,
    std::string code,
    std::string detail,
    SblrNativeSpecializationResult specialization = {}) {
  SblrHotPathExecutionResult result;
  result.ok = true;
  result.benchmark_clean = false;
  result.fallback_used = true;
  result.diagnostic_code = std::move(code);
  result.detail = std::move(detail);
  result.specialization = std::move(specialization);
  AddCommonEvidence(request, &result);
  result.evidence.push_back("sblr_hot_path.fallback_used=true");
  result.evidence.push_back("sblr_hot_path.exact_fallback_required=true");
  result.evidence.push_back("sblr_hot_path.exact_fallback_reason=" +
                            result.diagnostic_code);
  result.evidence.insert(result.evidence.end(),
                         result.specialization.evidence.begin(),
                         result.specialization.evidence.end());
  return result;
}

bool HasSuperinstructionReduction(
    const SblrHotPathSuperinstructionPlan& superinstruction) {
  return superinstruction.available && superinstruction.safe &&
         superinstruction.exact_scalar_fallback_available &&
         !superinstruction.superinstruction_id.empty() &&
         !superinstruction.fused_opcodes.empty() &&
         superinstruction.scalar_dispatches > superinstruction.fused_dispatches;
}

bool HasBatchReduction(const SblrHotPathBatchPlan& batch) {
  if (batch.repeated_rows == 0 || batch.scalar_dispatches_per_row == 0) {
    return false;
  }
  const std::uint64_t scalar_total =
      batch.repeated_rows * batch.scalar_dispatches_per_row;
  return batch.row_ordering_preserved &&
         batch.result_contract_hash_matches &&
         !batch.expected_result_contract_hash.empty() &&
         batch.expected_result_contract_hash == batch.observed_result_contract_hash &&
         scalar_total > batch.batched_dispatches_total;
}

std::uint64_t Saved(std::uint64_t before, std::uint64_t after) {
  return before > after ? before - after : 0;
}

void AppendMetricsEvidence(SblrHotPathExecutionResult* result,
                           const SblrExecutionMetrics& metrics,
                           const SblrHotPathProfilerEvidence& profiler) {
  for (const auto& sample : SnapshotSblrExecutionMetrics(metrics)) {
    result->evidence.push_back("sblr_hot_path.metric." + sample.metric_id +
                               "=" + std::to_string(sample.value));
  }
  result->evidence.push_back("sblr_hot_path.profiler_source_label=" +
                             profiler.source_label);
  result->evidence.push_back("sblr_hot_path.profiler_sample_count=" +
                             std::to_string(profiler.sample_count));
  result->evidence.push_back("sblr_hot_path.baseline_dispatch_us=" +
                             std::to_string(profiler.baseline_dispatch_us));
  result->evidence.push_back("sblr_hot_path.optimized_dispatch_us=" +
                             std::to_string(profiler.optimized_dispatch_us));
}

}  // namespace

SblrHotPathExecutionResult ExecuteSblrHotPath(
    const SblrHotPathExecutionRequest& request) {
  if (request.route_label.empty()) {
    return Refuse(request, "ORH_SBLR_HOT_PATH_ROUTE_LABEL_REQUIRED",
                  "route label is required");
  }
  if (request.authority.parser_sql_execution_authority ||
      request.authority.reference_execution_authority ||
      request.authority.client_execution_authority ||
      request.envelope.contains_sql_text ||
      request.envelope.source_artifact_map.contains_sql_text ||
      request.envelope.source_artifact_map.raw_sql_text_authoritative) {
    return Refuse(request, "ORH_SBLR_HOT_PATH_EXTERNAL_AUTHORITY_REFUSED",
                  "parser SQL, reference, and client routes cannot execute or own "
                  "SBLR hot-path authority");
  }
  if (request.authority.template_visibility_or_finality_authority ||
      request.authority.specialization_visibility_or_finality_authority ||
      request.authority.superinstruction_visibility_or_finality_authority ||
      request.authority.batch_visibility_or_finality_authority) {
    return Refuse(request, "ORH_SBLR_HOT_PATH_EXTERNAL_AUTHORITY_REFUSED",
                  "templates, specialization, superinstructions, and batches "
                  "are not visibility or finality authority");
  }
  if (request.envelope.operation_id.empty() || request.envelope.opcode.empty()) {
    return Refuse(request, "ORH_SBLR_HOT_PATH_ENVELOPE_PROOF_MISSING",
                  "SBLR operation envelope proof is required");
  }
  if (!request.authority.engine_mga_snapshot_bound ||
      !request.authority.transaction_inventory_authoritative ||
      request.context.snapshot_visible_through_local_transaction_id == 0 ||
      request.context.local_transaction_id == 0) {
    return Refuse(request, "ORH_SBLR_HOT_PATH_MGA_UNPROVEN",
                  "engine MGA snapshot and transaction inventory proof are "
                  "required");
  }
  if (!request.authority.security_recheck_required ||
      !request.context.security_context_present) {
    return Refuse(request, "ORH_SBLR_HOT_PATH_SECURITY_UNPROVEN",
                  "security context and recheck evidence are required");
  }
  if (request.template_cache == nullptr) {
    return Refuse(request, "ORH_SBLR_HOT_PATH_TEMPLATE_CACHE_REQUIRED",
                  "prepared template cache is required");
  }
  if (!request.profiler.measured || request.profiler.source_label.empty() ||
      request.profiler.sample_count == 0 ||
      request.profiler.optimized_dispatch_us >=
          request.profiler.baseline_dispatch_us) {
    return Refuse(request, "ORH_SBLR_HOT_PATH_PROFILER_MISSING",
                  "measured profiler evidence with dispatch reduction is "
                  "required");
  }
  if (!request.superinstruction.exact_scalar_fallback_available) {
    return Refuse(request, "ORH_SBLR_HOT_PATH_EXACT_FALLBACK_UNPROVEN",
                  "exact scalar fallback is required before superinstruction "
                  "or specialization admission");
  }
  if (!HasSuperinstructionReduction(request.superinstruction)) {
    return Fallback(request, "ORH_SBLR_HOT_PATH_UNSAFE_SUPERINSTRUCTION",
                    "superinstruction is unavailable, unsafe, or not faster");
  }
  if (!HasBatchReduction(request.batch)) {
    return Refuse(request, "ORH_SBLR_HOT_PATH_RESULT_CONTRACT_MISMATCH",
                  "batched repeated-row execution must preserve ordering and "
                  "result contract hash");
  }

  auto build = BuildPreparedTemplateFromSblr(
      request.envelope, request.context, request.api_request);
  if (!build.ok) {
    return Refuse(request, build.diagnostic_code, build.detail);
  }

  SblrHotPathExecutionResult result;
  auto admission = build.admission;
  result.first_prepare = request.template_cache->Prepare(admission);
  if (!result.first_prepare.ok || !result.first_prepare.prepared_template) {
    return Refuse(request, result.first_prepare.diagnostic_code,
                  result.first_prepare.detail);
  }
  result.reused_prepare = request.template_cache->Prepare(build.admission);
  if (!result.reused_prepare.ok ||
      !result.reused_prepare.reused_existing_template ||
      !result.reused_prepare.prepared_template) {
    return Refuse(request, "ORH_SBLR_HOT_PATH_TEMPLATE_REUSE_UNPROVEN",
                  "prepared template cache reuse was not observed");
  }
  result.bind = request.template_cache->Bind(
      *result.reused_prepare.prepared_template, build.bind_context);
  if (!result.bind.ok) {
    return Refuse(request, result.bind.diagnostic_code, result.bind.detail);
  }

  auto native_request = request.native_specialization;
  native_request.identity.stable_template_id =
      native_request.identity.stable_template_id.empty()
          ? result.reused_prepare.prepared_template->template_id
          : native_request.identity.stable_template_id;
  native_request.identity.sblr_digest =
      native_request.identity.sblr_digest.empty()
          ? result.reused_prepare.prepared_template->key.sblr_digest_or_trace_key
          : native_request.identity.sblr_digest;
  native_request.identity.template_generation =
      native_request.identity.template_generation == 0
          ? request.context.catalog_generation_id
          : native_request.identity.template_generation;
  native_request.identity.expected_template_generation =
      native_request.identity.expected_template_generation == 0
          ? native_request.identity.template_generation
          : native_request.identity.expected_template_generation;
  native_request.epochs.security_epoch =
      native_request.epochs.security_epoch == 0
          ? request.context.security_epoch
          : native_request.epochs.security_epoch;
  native_request.epochs.expected_security_epoch =
      native_request.epochs.expected_security_epoch == 0
          ? native_request.epochs.security_epoch
          : native_request.epochs.expected_security_epoch;
  native_request.epochs.redaction_epoch =
      native_request.epochs.redaction_epoch == 0
          ? request.context.resource_epoch
          : native_request.epochs.redaction_epoch;
  native_request.epochs.expected_redaction_epoch =
      native_request.epochs.expected_redaction_epoch == 0
          ? native_request.epochs.redaction_epoch
          : native_request.epochs.expected_redaction_epoch;

  result.specialization = ExecuteSblrNativeSpecialization(native_request);
  if (!result.specialization.ok) {
    result.fail_closed = true;
    result.diagnostic_code = result.specialization.diagnostic_code;
    result.detail = result.specialization.diagnostic_detail;
    AddCommonEvidence(request, &result);
    result.evidence.push_back("sblr_hot_path.fail_closed=true");
    result.evidence.insert(result.evidence.end(),
                           result.specialization.evidence.begin(),
                           result.specialization.evidence.end());
    return result;
  }
  if (!result.specialization.native_used) {
    return Fallback(request, result.specialization.diagnostic_code,
                    result.specialization.diagnostic_detail,
                    result.specialization);
  }

  SblrExecutionMetrics metrics;
  RecordSblrOpcodeDispatchMetric(&metrics, request.envelope.operation_id,
                                 request.profiler.baseline_dispatch_us);
  RecordSblrFunctionCallMetric(&metrics, request.superinstruction.superinstruction_id,
                               request.profiler.optimized_dispatch_us);

  const std::uint64_t scalar_batch_dispatches =
      request.batch.repeated_rows * request.batch.scalar_dispatches_per_row;
  result.dispatch_us_saved = Saved(request.profiler.baseline_dispatch_us,
                                   request.profiler.optimized_dispatch_us);
  result.opcode_dispatches_saved =
      Saved(request.superinstruction.scalar_dispatches,
            request.superinstruction.fused_dispatches) +
      Saved(scalar_batch_dispatches, request.batch.batched_dispatches_total);
  result.ok = true;
  result.benchmark_clean = true;
  result.diagnostic_code = "ORH_SBLR_HOT_PATH_OK";
  result.detail = "prepared SBLR hot path consumed";
  AddCommonEvidence(request, &result);
  result.evidence.push_back("sblr_hot_path.benchmark_clean=true");
  result.evidence.push_back("sblr_hot_path.prepared_template_id=" +
                            result.reused_prepare.prepared_template->template_id);
  result.evidence.push_back("sblr_hot_path.prepared_template_reused=true");
  result.evidence.push_back("sblr_hot_path.opcode_specialization=native");
  result.evidence.push_back("sblr_hot_path.superinstruction_id=" +
                            request.superinstruction.superinstruction_id);
  result.evidence.push_back("sblr_hot_path.batched_repeated_rows=" +
                            std::to_string(request.batch.repeated_rows));
  result.evidence.push_back("sblr_hot_path.result_contract_hash=" +
                            request.batch.observed_result_contract_hash);
  result.evidence.push_back("sblr_hot_path.dispatch_us_saved=" +
                            std::to_string(result.dispatch_us_saved));
  result.evidence.push_back("sblr_hot_path.opcode_dispatches_saved=" +
                            std::to_string(result.opcode_dispatches_saved));
  result.evidence.insert(result.evidence.end(), build.evidence.begin(),
                         build.evidence.end());
  result.evidence.insert(result.evidence.end(),
                         result.bind.evidence.begin(),
                         result.bind.evidence.end());
  result.evidence.insert(result.evidence.end(),
                         result.specialization.evidence.begin(),
                         result.specialization.evidence.end());
  AppendMetricsEvidence(&result, metrics, request.profiler);
  return result;
}

}  // namespace scratchbird::engine::sblr
