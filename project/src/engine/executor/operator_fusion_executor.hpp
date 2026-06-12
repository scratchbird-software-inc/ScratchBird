// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#pragma once

// SB-EXECUTOR-OPERATOR-FUSION-ODF-096-ANCHOR
// Operator fusion keeps candidate-producing pipelines in executor-owned
// candidate/vectorized form until exact, MGA, and security rechecks complete.

#include "candidate_set.hpp"
#include "runtime_filter_executor.hpp"
#include "runtime_platform.hpp"
#include "vectorized_result_batch.hpp"

#include <functional>
#include <string>
#include <vector>

namespace scratchbird::engine::executor {

enum class OperatorFusionPipelineKind {
  kUnknown,
  kScanFilterProject,
  kIndexVisibilityProject,
  kSearchScoreTopK,
  kVectorRerank,
  kGraphFrontier,
  kTimeAggregate,
};

enum class OperatorFusionStageKind {
  kUnknown,
  kScan,
  kFilter,
  kProject,
  kIndexProbe,
  kVisibilityRecheck,
  kSearchCandidate,
  kScore,
  kTopK,
  kVectorCandidate,
  kRerank,
  kGraphSeed,
  kGraphFrontier,
  kTimeBucketScan,
  kAggregate,
};

struct OperatorFusionPipelinePlan {
  OperatorFusionPipelineKind kind = OperatorFusionPipelineKind::kUnknown;
  std::string pipeline_id;
  std::string plan_node_id;
  std::string provider_id;
  std::string descriptor_id;
  std::string expected_row_descriptor_id;
  std::string output_descriptor_id;
  std::vector<OperatorFusionStageKind> stages;
  std::vector<std::string> projected_column_names;
  std::vector<scratchbird::engine::optimizer::RuntimeFilterDescriptor>
      runtime_filters;

  scratchbird::core::platform::u64 descriptor_generation = 0;
  scratchbird::core::platform::u64 required_descriptor_generation = 0;
  scratchbird::core::platform::u64 input_rows = 0;
  scratchbird::core::platform::u64 limit_k = 0;

  bool fusion_supported = true;
  bool physical_provider_selected = true;
  bool descriptor_scan_selected = false;
  bool behavior_store_scan_selected = false;
  bool stale = false;
  bool lossy_or_approximate = false;
  bool exact_fallback_available = true;

  bool engine_mga_authoritative = true;
  bool security_context_bound = true;
  bool exact_recheck_required = true;
  bool mga_visibility_recheck_required = true;
  bool security_authorization_recheck_required = true;
  bool exact_rerank_source_available = true;

  bool redaction_barrier_crossed = false;
  bool redaction_barrier_proven = true;
  bool security_barrier_crossed = false;
  bool security_barrier_proven = true;

  bool parser_or_reference_finality_or_visibility_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool write_ahead_log_finality_or_visibility_authority = false;
};

struct OperatorFusionProviderRequest {
  OperatorFusionPipelinePlan plan;
  scratchbird::core::index::CandidateSetAuthorityContext authority;
};

struct OperatorFusionProviderResult {
  scratchbird::core::platform::Status status;
  bool fail_closed = false;
  bool unsupported = false;
  bool returns_final_rows = false;
  bool exact_recheck_evidence_present = false;
  bool mga_recheck_evidence_present = false;
  bool security_recheck_evidence_present = false;
  bool provider_authority_evidence_present = false;
  bool redaction_or_security_violation = false;
  bool parser_or_reference_finality_or_visibility_authority = false;
  bool client_finality_or_visibility_authority = false;
  bool provider_finality_or_visibility_authority = false;
  bool write_ahead_log_finality_or_visibility_authority = false;
  scratchbird::core::platform::u64 descriptor_generation = 0;
  std::string row_descriptor_id;
  std::vector<std::string> projected_column_names;
  std::vector<scratchbird::core::index::CandidateSetRow> candidate_rows;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

using OperatorFusionProvider =
    std::function<OperatorFusionProviderResult(
        const OperatorFusionProviderRequest&)>;
using OperatorFusionRerankScorer =
    std::function<double(const scratchbird::core::index::CandidateSetRow&)>;

struct OperatorFusionProviderSet {
  OperatorFusionProvider primary_provider;
  OperatorFusionProvider exact_fallback_provider;
  RuntimeFilterProviderSet runtime_filter_providers;
  OperatorFusionRerankScorer rerank_scorer;
};

struct OperatorFusionCounters {
  scratchbird::core::platform::u64 input_rows = 0;
  scratchbird::core::platform::u64 candidate_rows = 0;
  scratchbird::core::platform::u64 output_rows = 0;
  scratchbird::core::platform::u64 fused_stages = 0;
  scratchbird::core::platform::u64 materialization_barriers_avoided = 0;
  scratchbird::core::platform::u64 fallback_count = 0;
  scratchbird::core::platform::u64 materialization_count = 0;
  scratchbird::core::platform::u64 runtime_filter_use_count = 0;
  scratchbird::core::platform::u64 exact_recheck_count = 0;
  scratchbird::core::platform::u64 mga_recheck_count = 0;
  scratchbird::core::platform::u64 security_recheck_count = 0;
  scratchbird::core::platform::u64 provider_authority_evidence_count = 0;
};

struct OperatorFusionExecutionResult {
  scratchbird::core::platform::Status status;
  bool fail_closed = false;
  std::string diagnostic_code;
  OperatorFusionCounters counters;
  scratchbird::core::index::CandidateSet candidate_rows;
  std::vector<scratchbird::core::platform::TypedUuid> final_row_uuids;
  VectorizedResultBatch output_batch;
  std::vector<std::string> evidence;

  bool ok() const { return status.ok() && !fail_closed; }
};

const char* OperatorFusionPipelineKindName(OperatorFusionPipelineKind kind);
const char* OperatorFusionStageKindName(OperatorFusionStageKind stage);

OperatorFusionExecutionResult ExecuteOperatorFusionPipeline(
    const OperatorFusionPipelinePlan& plan,
    const scratchbird::core::index::CandidateSetAuthorityContext& authority,
    const OperatorFusionProviderSet& providers);

OperatorFusionProviderResult MakeUnsupportedOperatorFusionProviderResult(
    std::string evidence);

}  // namespace scratchbird::engine::executor
