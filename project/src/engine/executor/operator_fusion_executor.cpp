// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "operator_fusion_executor.hpp"

#include "candidate_set_executor.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace scratchbird::engine::executor {
namespace {

namespace idx = scratchbird::core::index;
namespace opt = scratchbird::engine::optimizer;
namespace platform = scratchbird::core::platform;

platform::Status OkStatus() {
  return {platform::StatusCode::ok, platform::Severity::info,
          platform::Subsystem::engine};
}

platform::Status RefusalStatus() {
  return {platform::StatusCode::diagnostic_invalid_record,
          platform::Severity::error, platform::Subsystem::engine};
}

void Add(std::vector<std::string>* evidence, std::string value) {
  evidence->push_back(std::move(value));
}

OperatorFusionExecutionResult Refuse(std::string code, std::string evidence) {
  OperatorFusionExecutionResult result;
  result.status = RefusalStatus();
  result.fail_closed = true;
  result.diagnostic_code = std::move(code);
  Add(&result.evidence, std::move(evidence));
  Add(&result.evidence, "operator_fusion.executor.fail_closed=true");
  Add(&result.evidence, "operator_fusion.candidate_rows_only_until_recheck=true");
  Add(&result.evidence, "operator_fusion.exact_recheck_required=true");
  Add(&result.evidence, "operator_fusion.mga_visibility_recheck_required=true");
  Add(&result.evidence, "operator_fusion.security_recheck_required=true");
  Add(&result.evidence, "parser_or_reference_finality_or_visibility_authority=false");
  Add(&result.evidence, "client_finality_or_visibility_authority=false");
  Add(&result.evidence, "provider_finality_or_visibility_authority=false");
  Add(&result.evidence,
      "write_ahead_log_finality_or_visibility_authority=false");
  return result;
}

idx::CandidateSetAuthorityContext PlanAuthority(
    const OperatorFusionPipelinePlan& plan,
    idx::CandidateSetAuthorityContext authority) {
  authority.engine_mga_authoritative =
      authority.engine_mga_authoritative && plan.engine_mga_authoritative;
  authority.security_context_bound =
      authority.security_context_bound && plan.security_context_bound;
  authority.row_mga_recheck_required =
      authority.row_mga_recheck_required &&
      plan.mga_visibility_recheck_required;
  authority.row_security_recheck_required =
      authority.row_security_recheck_required &&
      plan.security_authorization_recheck_required;
  authority.exact_recheck_available =
      authority.exact_recheck_available && plan.exact_recheck_required;
  authority.exact_rerank_source_available =
      authority.exact_rerank_source_available &&
      plan.exact_rerank_source_available;
  authority.parser_or_reference_finality_or_visibility_authority =
      authority.parser_or_reference_finality_or_visibility_authority ||
      plan.parser_or_reference_finality_or_visibility_authority;
  authority.client_finality_or_visibility_authority =
      authority.client_finality_or_visibility_authority ||
      plan.client_finality_or_visibility_authority;
  authority.provider_finality_or_visibility_authority =
      authority.provider_finality_or_visibility_authority ||
      plan.provider_finality_or_visibility_authority;
  authority.wal_recovery_or_finality_authority =
      authority.wal_recovery_or_finality_authority ||
      plan.write_ahead_log_finality_or_visibility_authority;
  return authority;
}

bool SameStages(const std::vector<OperatorFusionStageKind>& stages,
                std::initializer_list<OperatorFusionStageKind> expected) {
  return stages.size() == expected.size() &&
         std::equal(stages.begin(), stages.end(), expected.begin());
}

bool SupportedShape(const OperatorFusionPipelinePlan& plan) {
  switch (plan.kind) {
    case OperatorFusionPipelineKind::kScanFilterProject:
      return SameStages(plan.stages, {OperatorFusionStageKind::kScan,
                                      OperatorFusionStageKind::kFilter,
                                      OperatorFusionStageKind::kProject});
    case OperatorFusionPipelineKind::kIndexVisibilityProject:
      return SameStages(plan.stages, {OperatorFusionStageKind::kIndexProbe,
                                      OperatorFusionStageKind::kVisibilityRecheck,
                                      OperatorFusionStageKind::kProject});
    case OperatorFusionPipelineKind::kSearchScoreTopK:
      return SameStages(plan.stages, {OperatorFusionStageKind::kSearchCandidate,
                                      OperatorFusionStageKind::kScore,
                                      OperatorFusionStageKind::kTopK});
    case OperatorFusionPipelineKind::kVectorRerank:
      return SameStages(plan.stages, {OperatorFusionStageKind::kVectorCandidate,
                                      OperatorFusionStageKind::kRerank,
                                      OperatorFusionStageKind::kProject});
    case OperatorFusionPipelineKind::kGraphFrontier:
      return SameStages(plan.stages, {OperatorFusionStageKind::kGraphSeed,
                                      OperatorFusionStageKind::kGraphFrontier,
                                      OperatorFusionStageKind::kProject});
    case OperatorFusionPipelineKind::kTimeAggregate:
      return SameStages(plan.stages, {OperatorFusionStageKind::kTimeBucketScan,
                                      OperatorFusionStageKind::kAggregate,
                                      OperatorFusionStageKind::kProject});
    case OperatorFusionPipelineKind::kUnknown:
      break;
  }
  return false;
}

bool UnsafeProviderAuthority(const OperatorFusionProviderResult& result) {
  return result.parser_or_reference_finality_or_visibility_authority ||
         result.client_finality_or_visibility_authority ||
         result.provider_finality_or_visibility_authority ||
         result.write_ahead_log_finality_or_visibility_authority;
}

OperatorFusionExecutionResult ValidatePlan(
    const OperatorFusionPipelinePlan& plan,
    const idx::CandidateSetAuthorityContext& authority) {
  if (plan.pipeline_id.empty() || plan.plan_node_id.empty() ||
      plan.provider_id.empty() || plan.descriptor_id.empty() ||
      plan.expected_row_descriptor_id.empty() ||
      plan.output_descriptor_id.empty()) {
    return Refuse("SB_OPERATOR_FUSION.DESCRIPTOR_REQUIRED",
                  "operator_fusion_descriptor_identity_required");
  }
  if (!plan.fusion_supported || !SupportedShape(plan)) {
    return Refuse("SB_OPERATOR_FUSION.UNSUPPORTED_PIPELINE",
                  "operator_fusion_pipeline_shape_unsupported");
  }
  if (!plan.physical_provider_selected || plan.descriptor_scan_selected ||
      plan.behavior_store_scan_selected) {
    return Refuse("SB_OPERATOR_FUSION.PHYSICAL_PROVIDER_REQUIRED",
                  "descriptor_or_behavior_store_scan_refused");
  }
  if (plan.stale || plan.descriptor_generation <
                        plan.required_descriptor_generation) {
    return Refuse("SB_OPERATOR_FUSION.STALE_DESCRIPTOR",
                  "stale_operator_fusion_descriptor");
  }
  if (plan.input_rows == 0 || plan.projected_column_names.empty()) {
    return Refuse("SB_OPERATOR_FUSION.DESCRIPTOR_REQUIRED",
                  "input_rows_and_projection_required");
  }
  if (plan.lossy_or_approximate && !plan.exact_fallback_available) {
    return Refuse("SB_OPERATOR_FUSION.EXACT_FALLBACK_REQUIRED",
                  "lossy_pipeline_requires_exact_fallback");
  }
  if (plan.redaction_barrier_crossed && !plan.redaction_barrier_proven) {
    return Refuse("SB_OPERATOR_FUSION.REDACTION_BARRIER_REQUIRED",
                  "redaction_barrier_proof_required");
  }
  if (plan.security_barrier_crossed && !plan.security_barrier_proven) {
    return Refuse("SB_OPERATOR_FUSION.SECURITY_BARRIER_REQUIRED",
                  "security_barrier_proof_required");
  }
  const auto scoped_authority = PlanAuthority(plan, authority);
  if (scoped_authority.parser_or_reference_finality_or_visibility_authority ||
      scoped_authority.client_finality_or_visibility_authority ||
      scoped_authority.provider_finality_or_visibility_authority ||
      scoped_authority.wal_recovery_or_finality_authority) {
    return Refuse("SB_OPERATOR_FUSION.UNSAFE_AUTHORITY",
                  "unsafe_visibility_or_finality_authority");
  }
  if (!scoped_authority.engine_mga_authoritative ||
      !scoped_authority.row_mga_recheck_required) {
    return Refuse("SB_OPERATOR_FUSION.MGA_RECHECK_REQUIRED",
                  "engine_mga_visibility_recheck_required");
  }
  if (!scoped_authority.security_context_bound ||
      !scoped_authority.row_security_recheck_required) {
    return Refuse("SB_OPERATOR_FUSION.SECURITY_RECHECK_REQUIRED",
                  "security_authorization_recheck_required");
  }
  if (!scoped_authority.exact_recheck_available) {
    return Refuse("SB_OPERATOR_FUSION.EXACT_RECHECK_REQUIRED",
                  "exact_recheck_required");
  }
  OperatorFusionExecutionResult ok;
  ok.status = OkStatus();
  return ok;
}

OperatorFusionExecutionResult ValidateProviderRows(
    const OperatorFusionPipelinePlan& plan,
    const OperatorFusionProviderResult& provider_result) {
  if (provider_result.unsupported) {
    return Refuse("SB_OPERATOR_FUSION.PROVIDER_UNSUPPORTED",
                  "operator_fusion_provider_unsupported");
  }
  if (!provider_result.ok()) {
    return Refuse("SB_OPERATOR_FUSION.PROVIDER_FAILED",
                  "operator_fusion_provider_failed");
  }
  if (provider_result.descriptor_generation <
      plan.required_descriptor_generation) {
    return Refuse("SB_OPERATOR_FUSION.STALE_DESCRIPTOR",
                  "provider_descriptor_generation_stale");
  }
  if (provider_result.row_descriptor_id != plan.expected_row_descriptor_id ||
      provider_result.projected_column_names != plan.projected_column_names) {
    return Refuse("SB_OPERATOR_FUSION.ROW_DESCRIPTOR_MISMATCH",
                  "provider_row_descriptor_mismatch");
  }
  if (provider_result.redaction_or_security_violation) {
    return Refuse("SB_OPERATOR_FUSION.SECURITY_BARRIER_REQUIRED",
                  "provider_redaction_or_security_barrier_violation");
  }
  if (provider_result.candidate_rows.size() > plan.input_rows) {
    return Refuse("SB_OPERATOR_FUSION.COUNTERS_INVALID",
                  "provider_candidate_rows_exceed_input_rows");
  }
  if (UnsafeProviderAuthority(provider_result)) {
    return Refuse("SB_OPERATOR_FUSION.UNSAFE_AUTHORITY",
                  "provider_visibility_or_finality_claim_refused");
  }
  if (provider_result.returns_final_rows &&
      (!provider_result.exact_recheck_evidence_present ||
       !provider_result.mga_recheck_evidence_present ||
       !provider_result.security_recheck_evidence_present)) {
    return Refuse("SB_OPERATOR_FUSION.PROVIDER_FINAL_ROWS_RECHECK_REQUIRED",
                  "provider_final_rows_without_exact_mga_security_recheck");
  }
  if (!provider_result.exact_recheck_evidence_present) {
    return Refuse("SB_OPERATOR_FUSION.EXACT_RECHECK_EVIDENCE_REQUIRED",
                  "provider_exact_recheck_evidence_missing");
  }
  if (!provider_result.mga_recheck_evidence_present) {
    return Refuse("SB_OPERATOR_FUSION.MGA_RECHECK_EVIDENCE_REQUIRED",
                  "provider_mga_recheck_evidence_missing");
  }
  if (!provider_result.security_recheck_evidence_present) {
    return Refuse("SB_OPERATOR_FUSION.SECURITY_RECHECK_EVIDENCE_REQUIRED",
                  "provider_security_recheck_evidence_missing");
  }
  if (!provider_result.provider_authority_evidence_present) {
    return Refuse("SB_OPERATOR_FUSION.PROVIDER_AUTHORITY_EVIDENCE_REQUIRED",
                  "provider_authority_evidence_missing");
  }
  OperatorFusionExecutionResult ok;
  ok.status = OkStatus();
  return ok;
}

void AddPlanEvidence(OperatorFusionExecutionResult* result,
                     const OperatorFusionPipelinePlan& plan) {
  Add(&result->evidence, std::string("operator_fusion.pipeline=") +
                             OperatorFusionPipelineKindName(plan.kind));
  Add(&result->evidence, "operator_fusion.pipeline_id=" + plan.pipeline_id);
  Add(&result->evidence, "operator_fusion.plan_node_id=" + plan.plan_node_id);
  Add(&result->evidence, "operator_fusion.provider_id=" + plan.provider_id);
  Add(&result->evidence,
      "operator_fusion.input_rows=" + std::to_string(plan.input_rows));
  for (const auto stage : plan.stages) {
    Add(&result->evidence, std::string("operator_fusion.fused_stage=") +
                               OperatorFusionStageKindName(stage));
  }
}

std::vector<std::uint8_t> FixedBytes(platform::u64 row_count,
                                     platform::u64 width,
                                     std::uint8_t seed) {
  std::vector<std::uint8_t> bytes;
  bytes.resize(static_cast<std::size_t>(row_count * width));
  for (std::size_t i = 0; i < bytes.size(); ++i) {
    bytes[i] = static_cast<std::uint8_t>(seed + (i % 17u));
  }
  return bytes;
}

VectorizedResultBatchResult BuildOutputBatch(
    const OperatorFusionPipelinePlan& plan,
    platform::u64 row_count) {
  VectorizedResultBatchBuilder builder(row_count);
  std::uint8_t seed = 0x60;
  for (const auto& column : plan.projected_column_names) {
    builder.AddColumn(MakeFixedWidthResultBatchColumn(
        column, row_count, 8, FixedBytes(row_count, 8, seed),
        MakeResultBatchValidityBitmap(row_count)));
    seed = static_cast<std::uint8_t>(seed + 11u);
  }
  return builder.Finalize();
}

OperatorFusionExecutionResult ApplyPipelineCandidateOperations(
    const OperatorFusionPipelinePlan& plan,
    const idx::CandidateSet& input,
    const idx::CandidateSetAuthorityContext& authority,
    const OperatorFusionProviderSet& providers) {
  if (plan.kind == OperatorFusionPipelineKind::kSearchScoreTopK) {
    if (plan.limit_k == 0) {
      return Refuse("SB_OPERATOR_FUSION.TOP_K_REQUIRED",
                    "search_score_top_k_requires_limit");
    }
    auto top_k = idx::TopKCandidateSet(input, plan.limit_k, authority);
    if (!top_k.ok()) {
      auto refused = Refuse(top_k.diagnostic.diagnostic_code,
                            "operator_fusion_top_k_refused");
      refused.evidence.insert(refused.evidence.end(), top_k.evidence.begin(),
                              top_k.evidence.end());
      return refused;
    }
    OperatorFusionExecutionResult ok;
    ok.status = OkStatus();
    ok.candidate_rows = std::move(top_k.output);
    ok.evidence = std::move(top_k.evidence);
    return ok;
  }

  if (plan.kind == OperatorFusionPipelineKind::kVectorRerank) {
    if (!providers.rerank_scorer) {
      return Refuse("SB_OPERATOR_FUSION.EXACT_FALLBACK_REQUIRED",
                    "vector_rerank_exact_scorer_required");
    }
    auto reranked = idx::RerankCandidateSet(input, providers.rerank_scorer,
                                            authority);
    if (!reranked.ok()) {
      auto refused = Refuse(reranked.diagnostic.diagnostic_code,
                            "operator_fusion_rerank_refused");
      refused.evidence.insert(refused.evidence.end(), reranked.evidence.begin(),
                              reranked.evidence.end());
      return refused;
    }
    OperatorFusionExecutionResult ok;
    ok.status = OkStatus();
    ok.candidate_rows = std::move(reranked.output);
    ok.evidence = std::move(reranked.evidence);
    return ok;
  }

  OperatorFusionExecutionResult ok;
  ok.status = OkStatus();
  ok.candidate_rows = input;
  return ok;
}

}  // namespace

const char* OperatorFusionPipelineKindName(OperatorFusionPipelineKind kind) {
  switch (kind) {
    case OperatorFusionPipelineKind::kScanFilterProject:
      return "scan_filter_project";
    case OperatorFusionPipelineKind::kIndexVisibilityProject:
      return "index_visibility_project";
    case OperatorFusionPipelineKind::kSearchScoreTopK:
      return "search_score_top_k";
    case OperatorFusionPipelineKind::kVectorRerank:
      return "vector_rerank";
    case OperatorFusionPipelineKind::kGraphFrontier:
      return "graph_frontier";
    case OperatorFusionPipelineKind::kTimeAggregate:
      return "time_aggregate";
    case OperatorFusionPipelineKind::kUnknown:
      break;
  }
  return "unknown";
}

const char* OperatorFusionStageKindName(OperatorFusionStageKind stage) {
  switch (stage) {
    case OperatorFusionStageKind::kScan:
      return "scan";
    case OperatorFusionStageKind::kFilter:
      return "filter";
    case OperatorFusionStageKind::kProject:
      return "project";
    case OperatorFusionStageKind::kIndexProbe:
      return "index_probe";
    case OperatorFusionStageKind::kVisibilityRecheck:
      return "visibility_recheck";
    case OperatorFusionStageKind::kSearchCandidate:
      return "search_candidate";
    case OperatorFusionStageKind::kScore:
      return "score";
    case OperatorFusionStageKind::kTopK:
      return "top_k";
    case OperatorFusionStageKind::kVectorCandidate:
      return "vector_candidate";
    case OperatorFusionStageKind::kRerank:
      return "rerank";
    case OperatorFusionStageKind::kGraphSeed:
      return "graph_seed";
    case OperatorFusionStageKind::kGraphFrontier:
      return "graph_frontier";
    case OperatorFusionStageKind::kTimeBucketScan:
      return "time_bucket_scan";
    case OperatorFusionStageKind::kAggregate:
      return "aggregate";
    case OperatorFusionStageKind::kUnknown:
      break;
  }
  return "unknown";
}

OperatorFusionProviderResult MakeUnsupportedOperatorFusionProviderResult(
    std::string evidence) {
  OperatorFusionProviderResult result;
  result.status = OkStatus();
  result.unsupported = true;
  result.exact_recheck_evidence_present = true;
  result.mga_recheck_evidence_present = true;
  result.security_recheck_evidence_present = true;
  result.provider_authority_evidence_present = true;
  Add(&result.evidence, std::move(evidence));
  Add(&result.evidence, "operator_fusion.provider_supported=false");
  return result;
}

OperatorFusionExecutionResult ExecuteOperatorFusionPipeline(
    const OperatorFusionPipelinePlan& plan,
    const idx::CandidateSetAuthorityContext& authority,
    const OperatorFusionProviderSet& providers) {
  const auto scoped_authority = PlanAuthority(plan, authority);
  auto plan_check = ValidatePlan(plan, authority);
  if (!plan_check.ok()) {
    return plan_check;
  }
  if (!providers.primary_provider) {
    return Refuse("SB_OPERATOR_FUSION.PROVIDER_REQUIRED",
                  "operator_fusion_provider_missing");
  }

  OperatorFusionExecutionResult result;
  result.status = OkStatus();
  result.diagnostic_code = "SB_OPERATOR_FUSION.OK";
  result.counters.input_rows = plan.input_rows;
  result.counters.fused_stages = plan.stages.size();
  result.counters.materialization_barriers_avoided =
      plan.stages.empty() ? 0 : static_cast<platform::u64>(plan.stages.size() - 1);
  Add(&result.evidence, "operator_fusion.executor=fusion_v1");
  Add(&result.evidence, "operator_fusion.materializes_between_stages=false");
  AddPlanEvidence(&result, plan);

  idx::CandidateSet runtime_candidates;
  bool have_runtime_candidates = false;
  if (!plan.runtime_filters.empty()) {
    auto runtime_filter_result = ExecuteRuntimeFilterPushdown(
        plan.runtime_filters, scoped_authority, providers.runtime_filter_providers);
    if (!runtime_filter_result.ok()) {
      auto refused = Refuse(runtime_filter_result.diagnostic_code,
                            "operator_fusion_runtime_filter_refused");
      refused.evidence.insert(refused.evidence.end(),
                              runtime_filter_result.evidence.begin(),
                              runtime_filter_result.evidence.end());
      return refused;
    }
    runtime_candidates = std::move(runtime_filter_result.candidate_rows);
    have_runtime_candidates = true;
    result.counters.runtime_filter_use_count =
        runtime_filter_result.counters.pushed_filter_count;
    result.counters.fallback_count += runtime_filter_result.counters.fallback_count;
    result.evidence.insert(result.evidence.end(),
                           runtime_filter_result.evidence.begin(),
                           runtime_filter_result.evidence.end());
  }

  OperatorFusionProvider provider = providers.primary_provider;
  auto provider_result = provider({plan, scoped_authority});
  if (provider_result.unsupported) {
    if (!plan.exact_fallback_available || !providers.exact_fallback_provider) {
      return Refuse("SB_OPERATOR_FUSION.PROVIDER_UNSUPPORTED",
                    "operator_fusion_provider_unsupported_no_fallback");
    }
    ++result.counters.fallback_count;
    Add(&result.evidence, "operator_fusion.fallback=exact_provider");
    provider_result = providers.exact_fallback_provider({plan, scoped_authority});
  }
  auto provider_check = ValidateProviderRows(plan, provider_result);
  if (!provider_check.ok()) {
    return provider_check;
  }
  if (provider_result.returns_final_rows) {
    Add(&result.evidence,
        "operator_fusion.provider_final_rows_demoted_to_candidates=true");
  }
  result.evidence.insert(result.evidence.end(), provider_result.evidence.begin(),
                         provider_result.evidence.end());
  if (provider_result.provider_authority_evidence_present) {
    ++result.counters.provider_authority_evidence_count;
  }

  auto provider_candidates = idx::MakeExactRowUuidOrderedCandidateSet(
      provider_result.candidate_rows, scoped_authority, false);
  if (!provider_candidates.ok()) {
    auto refused = Refuse(provider_candidates.diagnostic.diagnostic_code,
                          "operator_fusion_provider_candidate_rows_corrupt");
    refused.evidence.insert(refused.evidence.end(),
                            provider_candidates.evidence.begin(),
                            provider_candidates.evidence.end());
    return refused;
  }

  idx::CandidateSet fused_candidates = std::move(provider_candidates.output);
  if (have_runtime_candidates) {
    auto intersected = idx::IntersectCandidateSets(
        fused_candidates, runtime_candidates, scoped_authority);
    if (!intersected.ok()) {
      auto refused = Refuse(intersected.diagnostic.diagnostic_code,
                            "operator_fusion_runtime_filter_intersect_refused");
      refused.evidence.insert(refused.evidence.end(), intersected.evidence.begin(),
                              intersected.evidence.end());
      return refused;
    }
    fused_candidates = std::move(intersected.output);
    result.evidence.insert(result.evidence.end(), intersected.evidence.begin(),
                           intersected.evidence.end());
    Add(&result.evidence, "operator_fusion.runtime_filter_applied=true");
  }

  auto transformed = ApplyPipelineCandidateOperations(
      plan, fused_candidates, scoped_authority, providers);
  if (!transformed.ok()) {
    return transformed;
  }
  if (!transformed.evidence.empty()) {
    result.evidence.insert(result.evidence.end(), transformed.evidence.begin(),
                           transformed.evidence.end());
  }
  result.candidate_rows = std::move(transformed.candidate_rows);
  result.counters.candidate_rows = result.candidate_rows.rows.size();
  result.counters.exact_recheck_count = result.candidate_rows.rows.size();
  result.counters.mga_recheck_count = result.candidate_rows.rows.size();
  result.counters.security_recheck_count = result.candidate_rows.rows.size();

  auto finalized = FinalizeCandidateSetForExecutor(result.candidate_rows,
                                                   scoped_authority);
  if (!finalized.ok()) {
    auto refused = Refuse(finalized.recheck.diagnostic.diagnostic_code,
                          "operator_fusion_exact_mga_security_recheck_failed");
    refused.evidence.insert(refused.evidence.end(), finalized.evidence.begin(),
                            finalized.evidence.end());
    return refused;
  }
  result.final_row_uuids = std::move(finalized.final_row_uuids);
  result.evidence.insert(result.evidence.end(), finalized.evidence.begin(),
                         finalized.evidence.end());

  auto batch = BuildOutputBatch(plan, result.final_row_uuids.size());
  if (!batch.ok()) {
    auto refused = Refuse(batch.diagnostic.diagnostic_code,
                          "operator_fusion_vectorized_output_refused");
    refused.evidence.insert(refused.evidence.end(), batch.evidence.begin(),
                            batch.evidence.end());
    return refused;
  }
  result.output_batch = std::move(batch.batch);
  result.counters.output_rows = result.output_batch.row_count;
  result.evidence.insert(result.evidence.end(), batch.evidence.begin(),
                         batch.evidence.end());

  Add(&result.evidence,
      "operator_fusion.output_descriptor_id=" + plan.output_descriptor_id);
  Add(&result.evidence,
      "operator_fusion.input_rows=" + std::to_string(result.counters.input_rows));
  Add(&result.evidence,
      "operator_fusion.candidate_rows=" +
          std::to_string(result.counters.candidate_rows));
  Add(&result.evidence,
      "operator_fusion.output_rows=" +
          std::to_string(result.counters.output_rows));
  Add(&result.evidence,
      "operator_fusion.fused_stages=" +
          std::to_string(result.counters.fused_stages));
  Add(&result.evidence,
      "operator_fusion.materialization_barriers_avoided=" +
          std::to_string(result.counters.materialization_barriers_avoided));
  Add(&result.evidence,
      "operator_fusion.materialization_count=" +
          std::to_string(result.counters.materialization_count));
  Add(&result.evidence,
      "operator_fusion.fallback_count=" +
          std::to_string(result.counters.fallback_count));
  Add(&result.evidence,
      "operator_fusion.runtime_filter_use_count=" +
          std::to_string(result.counters.runtime_filter_use_count));
  Add(&result.evidence,
      "operator_fusion.exact_recheck_count=" +
          std::to_string(result.counters.exact_recheck_count));
  Add(&result.evidence,
      "operator_fusion.mga_recheck_count=" +
          std::to_string(result.counters.mga_recheck_count));
  Add(&result.evidence,
      "operator_fusion.security_recheck_count=" +
          std::to_string(result.counters.security_recheck_count));
  Add(&result.evidence,
      "operator_fusion.provider_authority_evidence_count=" +
          std::to_string(result.counters.provider_authority_evidence_count));
  Add(&result.evidence, "operator_fusion.exact_recheck_required=true");
  Add(&result.evidence, "operator_fusion.mga_visibility_recheck_required=true");
  Add(&result.evidence, "operator_fusion.security_recheck_required=true");
  Add(&result.evidence, "mga_finality_authority=engine_transaction_inventory");
  Add(&result.evidence, "parser_or_reference_finality_or_visibility_authority=false");
  Add(&result.evidence, "client_finality_or_visibility_authority=false");
  Add(&result.evidence, "provider_finality_or_visibility_authority=false");
  Add(&result.evidence,
      "write_ahead_log_finality_or_visibility_authority=false");
  return result;
}

}  // namespace scratchbird::engine::executor
