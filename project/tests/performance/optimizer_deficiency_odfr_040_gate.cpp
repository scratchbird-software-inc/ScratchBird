// Copyright (c) 2026 ScratchBird Software Inc.
//
// This Source Code Form is subject to the terms of the Mozilla Public
// License, v. 2.0. If a copy of the MPL was not distributed with this
// file, You can obtain one at https://mozilla.org/MPL/2.0/.
//
// SPDX-License-Identifier: MPL-2.0

#include "vector_index_generation_publication.hpp"
#include "vector_training_recall_lifecycle.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

namespace {

using scratchbird::core::index::DefaultVectorTrainingRecallLifecycleProfile;
using scratchbird::core::index::EvaluateVectorTrainingRecallLifecycle;
using scratchbird::core::index::IndexVectorAlgorithm;
using scratchbird::core::index::VectorGenerationRecallLifecycleEvidenceAdapter;
using scratchbird::core::index::VectorTrainingRecallLifecycleAction;
using scratchbird::core::index::VectorTrainingRecallLifecycleDecision;
using scratchbird::core::index::VectorTrainingRecallLifecycleProfile;
using scratchbird::core::index::kVectorTrainingRecallLifecycleSearchKey;

void Require(bool condition, const std::string& message) {
  if (!condition) {
    std::cerr << "ODFR-040 gate failure: " << message << '\n';
    std::exit(1);
  }
}

bool HasEvidence(const std::vector<std::string>& evidence,
                 const std::string& value) {
  return std::find(evidence.begin(), evidence.end(), value) != evidence.end();
}

bool HasEvidencePrefix(const std::vector<std::string>& evidence,
                       const std::string& prefix) {
  return std::any_of(evidence.begin(),
                     evidence.end(),
                     [&](const std::string& value) {
                       return value.rfind(prefix, 0) == 0;
                     });
}

void RequireBenchmarkSupportEvidence(
    const VectorTrainingRecallLifecycleDecision& decision) {
  const auto& evidence = decision.evidence;
  Require(HasEvidence(evidence, kVectorTrainingRecallLifecycleSearchKey),
          "missing ODFR-040 search key");
  Require(HasEvidencePrefix(evidence, "training_generation_id="),
          "missing training generation evidence");
  Require(HasEvidencePrefix(evidence, "vector_algorithm="),
          "missing algorithm evidence");
  Require(HasEvidencePrefix(evidence, "training_sample_rows="),
          "missing training sample rows evidence");
  Require(HasEvidencePrefix(evidence, "training_corpus_rows="),
          "missing training corpus rows evidence");
  Require(HasEvidencePrefix(evidence, "training_seed="),
          "missing deterministic seed evidence");
  Require(HasEvidencePrefix(evidence, "training_sample_fingerprint="),
          "missing sample fingerprint evidence");
  Require(HasEvidence(evidence, "training_set_selection_deterministic=true"),
          "missing deterministic training selection evidence");
  Require(HasEvidencePrefix(evidence, "ivf_centroid_count="),
          "missing IVF centroid evidence");
  Require(HasEvidencePrefix(evidence, "ivf_list_count="),
          "missing IVF list evidence");
  Require(HasEvidencePrefix(evidence, "ivf_list_imbalance_ratio="),
          "missing IVF imbalance evidence");
  Require(HasEvidencePrefix(evidence, "pq_codebook_count="),
          "missing PQ codebook evidence");
  Require(HasEvidencePrefix(evidence, "pq_subvector_count="),
          "missing PQ subvector evidence");
  Require(HasEvidencePrefix(evidence, "pq_bits_per_code="),
          "missing PQ bit evidence");
  Require(HasEvidencePrefix(evidence, "pq_quantization_error="),
          "missing quantization error evidence");
  Require(HasEvidencePrefix(evidence, "hnsw_m="),
          "missing HNSW M evidence");
  Require(HasEvidencePrefix(evidence, "hnsw_ef_construction="),
          "missing HNSW ef_construction evidence");
  Require(HasEvidencePrefix(evidence, "hnsw_ef_search="),
          "missing HNSW ef_search evidence");
  Require(HasEvidencePrefix(evidence, "recall_probe_set_size="),
          "missing recall probe set evidence");
  Require(HasEvidencePrefix(evidence, "recall_top_k="),
          "missing recall top_k evidence");
  Require(HasEvidencePrefix(evidence, "recall_required="),
          "missing required recall evidence");
  Require(HasEvidencePrefix(evidence, "recall_observed_ann="),
          "missing observed recall evidence");
  Require(HasEvidence(evidence,
                      "exact_fallback_sample_comparison_present=true"),
          "missing exact fallback sample comparison evidence");
  Require(HasEvidence(evidence, "exact_rerank_final_scoring_authority=true"),
          "missing exact rerank final scoring evidence");
  Require(HasEvidence(evidence, "base_row_mga_recheck_required=true"),
          "missing base-row MGA recheck evidence");
  Require(HasEvidence(evidence, "base_row_security_recheck_required=true"),
          "missing base-row security recheck evidence");
  Require(HasEvidence(evidence, "vector_metadata_visibility_authority=false"),
          "vector metadata visibility authority must be false");
  Require(HasEvidence(evidence, "vector_metadata_finality_authority=false"),
          "vector metadata finality authority must be false");
  Require(HasEvidence(evidence, "parser_or_reference_authority=false"),
          "parser/reference authority must be false");
  Require(HasEvidence(evidence, "write_ahead_or_finality_authority=false"),
          "write-ahead/finality authority must be false");
  Require(HasEvidence(evidence, "benchmark_clean=true"),
          "missing benchmark-clean evidence");
  Require(HasEvidence(evidence, "support_bundle_ready=true"),
          "missing support-bundle evidence");
  Require(HasEvidencePrefix(evidence, "vector_lifecycle_action="),
          "missing lifecycle action evidence");
}

void ProveHealthyProfilesKeepCurrentGeneration() {
  auto hnsw = DefaultVectorTrainingRecallLifecycleProfile(
      IndexVectorAlgorithm::hnsw);
  const auto hnsw_decision = EvaluateVectorTrainingRecallLifecycle(hnsw);
  Require(hnsw_decision.accepted && !hnsw_decision.fail_closed,
          "healthy HNSW profile was not accepted");
  Require(hnsw_decision.action ==
              VectorTrainingRecallLifecycleAction::kKeepCurrentGeneration,
          "healthy HNSW profile did not keep current generation");
  Require(HasEvidence(hnsw_decision.evidence, "hnsw_profile_present=true"),
          "healthy HNSW profile missing profile evidence");
  RequireBenchmarkSupportEvidence(hnsw_decision);

  auto ivf_pq = DefaultVectorTrainingRecallLifecycleProfile(
      IndexVectorAlgorithm::ivf_pq);
  const auto ivf_pq_decision = EvaluateVectorTrainingRecallLifecycle(ivf_pq);
  Require(ivf_pq_decision.accepted && !ivf_pq_decision.fail_closed,
          "healthy IVF/PQ profile was not accepted");
  Require(ivf_pq_decision.action ==
              VectorTrainingRecallLifecycleAction::kKeepCurrentGeneration,
          "healthy IVF/PQ profile did not keep current generation");
  Require(HasEvidence(ivf_pq_decision.evidence, "ivf_profile_present=true"),
          "healthy IVF profile missing profile evidence");
  Require(HasEvidence(ivf_pq_decision.evidence, "pq_profile_present=true"),
          "healthy PQ profile missing profile evidence");
  RequireBenchmarkSupportEvidence(ivf_pq_decision);

  const auto adapter_evidence =
      VectorGenerationRecallLifecycleEvidenceAdapter(ivf_pq_decision);
  Require(HasEvidence(adapter_evidence,
                      "vector_generation_publication_lifecycle_adapter=true"),
          "vector generation publication adapter evidence missing");
  Require(HasEvidence(adapter_evidence,
                      "vector_generation_metadata_visibility_authority=false"),
          "vector generation adapter must not claim visibility authority");
  Require(HasEvidence(adapter_evidence,
                      "vector_generation_metadata_finality_authority=false"),
          "vector generation adapter must not claim finality authority");
}

void ProveRecallDriftSchedulesRetrain() {
  auto profile = DefaultVectorTrainingRecallLifecycleProfile(
      IndexVectorAlgorithm::hnsw);
  profile.recall_probe.observed_ann_recall = 0.90;
  const auto decision = EvaluateVectorTrainingRecallLifecycle(profile);
  Require(decision.accepted && !decision.fail_closed,
          "low recall decision should schedule work, not refuse");
  Require(decision.action ==
              VectorTrainingRecallLifecycleAction::kScheduleRetrain,
          "low recall did not schedule retrain");
  Require(decision.diagnostic_code ==
              "INDEX.VECTOR_RECALL_LIFECYCLE.RECALL_DRIFT_RETRAIN",
          "low recall diagnostic mismatch");
  RequireBenchmarkSupportEvidence(decision);
}

void ProveCentroidImbalanceSchedulesRebuild() {
  auto profile = DefaultVectorTrainingRecallLifecycleProfile(
      IndexVectorAlgorithm::ivf_pq);
  profile.ivf.list_imbalance_ratio = 3.2;
  const auto decision = EvaluateVectorTrainingRecallLifecycle(profile);
  Require(decision.accepted && !decision.fail_closed,
          "centroid/list drift decision should schedule work, not refuse");
  Require(decision.action ==
              VectorTrainingRecallLifecycleAction::kScheduleRebuild,
          "centroid/list imbalance did not schedule rebuild");
  Require(decision.diagnostic_code ==
              "INDEX.VECTOR_RECALL_LIFECYCLE.STRUCTURE_DRIFT_REBUILD",
          "centroid/list drift diagnostic mismatch");
  RequireBenchmarkSupportEvidence(decision);
}

void ProveCorpusGenerationDriftSchedulesRebuild() {
  auto profile = DefaultVectorTrainingRecallLifecycleProfile(
      IndexVectorAlgorithm::ivf_pq);
  profile.drift.corpus_generation_drift = 2;
  profile.drift.max_corpus_generation_drift = 1;
  const auto decision = EvaluateVectorTrainingRecallLifecycle(profile);
  Require(decision.accepted && !decision.fail_closed,
          "corpus generation drift decision should schedule work, not refuse");
  Require(decision.action ==
              VectorTrainingRecallLifecycleAction::kScheduleRebuild,
          "corpus generation drift did not schedule rebuild");
  Require(decision.diagnostic_code ==
              "INDEX.VECTOR_RECALL_LIFECYCLE.STRUCTURE_DRIFT_REBUILD",
          "corpus generation drift diagnostic mismatch");
  RequireBenchmarkSupportEvidence(decision);
}

void ProveIncompleteAnnProfileUsesExactFallback() {
  auto profile = DefaultVectorTrainingRecallLifecycleProfile(
      IndexVectorAlgorithm::ivf_pq);
  profile.ivf.list_count = 0;
  const auto decision = EvaluateVectorTrainingRecallLifecycle(profile);
  Require(decision.accepted && !decision.fail_closed,
          "incomplete ANN profile with exact fallback should be accepted");
  Require(decision.action ==
              VectorTrainingRecallLifecycleAction::kExactFallback,
          "incomplete ANN profile did not select exact fallback");
  Require(decision.diagnostic_code ==
              "INDEX.VECTOR_RECALL_LIFECYCLE.IVF_PROFILE_EXACT_FALLBACK",
          "exact fallback diagnostic mismatch");
  Require(HasEvidence(decision.evidence, "exact_fallback_selected=true"),
          "exact fallback evidence missing");
  Require(HasEvidence(decision.evidence,
                      "ann_generation_not_used_as_authority=true"),
          "ANN authority suppression evidence missing");
  Require(HasEvidence(decision.evidence, "base_row_mga_recheck_required=true"),
          "exact fallback missing MGA recheck evidence");
  Require(HasEvidence(decision.evidence,
                      "base_row_security_recheck_required=true"),
          "exact fallback missing security recheck evidence");
  RequireBenchmarkSupportEvidence(decision);
}

void ProveQuantizationDriftSchedulesRetrain() {
  auto profile = DefaultVectorTrainingRecallLifecycleProfile(
      IndexVectorAlgorithm::ivf_pq);
  profile.pq.quantization_error = 0.18;
  const auto decision = EvaluateVectorTrainingRecallLifecycle(profile);
  Require(decision.accepted && !decision.fail_closed,
          "quantization drift decision should schedule work, not refuse");
  Require(decision.action ==
              VectorTrainingRecallLifecycleAction::kScheduleRetrain,
          "quantization drift did not schedule retrain");
  Require(decision.diagnostic_code ==
              "INDEX.VECTOR_RECALL_LIFECYCLE.QUANTIZATION_DRIFT_RETRAIN",
          "quantization drift diagnostic mismatch");
  RequireBenchmarkSupportEvidence(decision);
}

void RequireRefused(const VectorTrainingRecallLifecycleProfile& profile,
                    const std::string& diagnostic_code) {
  const auto decision = EvaluateVectorTrainingRecallLifecycle(profile);
  Require(!decision.accepted && decision.fail_closed,
          "unsafe profile was not refused");
  Require(decision.action == VectorTrainingRecallLifecycleAction::kRefuse,
          "unsafe profile did not produce refuse action");
  Require(decision.diagnostic_code == diagnostic_code,
          "unsafe profile diagnostic mismatch");
  Require(HasEvidence(decision.evidence, kVectorTrainingRecallLifecycleSearchKey),
          "refusal missing search-key evidence");
  Require(HasEvidence(decision.evidence, "support_bundle_ready=true"),
          "refusal missing support-bundle evidence");
}

void ProveUnsafeInputsRefused() {
  auto missing_exact = DefaultVectorTrainingRecallLifecycleProfile(
      IndexVectorAlgorithm::hnsw);
  missing_exact.exact_fallback_available = false;
  RequireRefused(
      missing_exact,
      "INDEX.VECTOR_RECALL_LIFECYCLE.EXACT_FALLBACK_REQUIRED");

  auto missing_rerank = DefaultVectorTrainingRecallLifecycleProfile(
      IndexVectorAlgorithm::hnsw);
  missing_rerank.recall_probe.exact_rerank_final_scoring_authority = false;
  RequireRefused(
      missing_rerank,
      "INDEX.VECTOR_RECALL_LIFECYCLE.EXACT_RERANK_REQUIRED");

  auto parser = DefaultVectorTrainingRecallLifecycleProfile(
      IndexVectorAlgorithm::hnsw);
  parser.parser_or_reference_authority = true;
  RequireRefused(
      parser,
      "INDEX.VECTOR_RECALL_LIFECYCLE.UNSAFE_PARSER_OR_REFERENCE_AUTHORITY");

  auto finality = DefaultVectorTrainingRecallLifecycleProfile(
      IndexVectorAlgorithm::hnsw);
  finality.write_ahead_or_finality_authority = true;
  RequireRefused(
      finality,
      "INDEX.VECTOR_RECALL_LIFECYCLE.UNSAFE_FINALITY_AUTHORITY");
}

}  // namespace

int main() {
  ProveHealthyProfilesKeepCurrentGeneration();
  ProveRecallDriftSchedulesRetrain();
  ProveCentroidImbalanceSchedulesRebuild();
  ProveCorpusGenerationDriftSchedulesRebuild();
  ProveIncompleteAnnProfileUsesExactFallback();
  ProveQuantizationDriftSchedulesRetrain();
  ProveUnsafeInputsRefused();
  return 0;
}
